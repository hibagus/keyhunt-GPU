#include "sqlite.h"
#include "schema_v1.h"
#include "keyhunt/crypto/hash/sha256.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace keyhunt::storage::detail {
namespace fs=std::filesystem;
namespace {
constexpr int application_id=0x4b484a31;
[[noreturn]] void error(sqlite3* db,const std::string& operation) {
    throw std::runtime_error(operation+": "+(db?sqlite3_errmsg(db):"SQLite initialization failed"));
}
void check_result(sqlite3* db,int result,const char* op){if(result!=SQLITE_OK)error(db,op);}
int64_t scalar(sqlite3* db,const std::string& sql){Statement s(db,sql);if(!s.step())throw std::runtime_error("missing SQLite scalar");return s.integer(0);}
bool inside(fs::path path,const fs::path& root){for(;!path.empty();path=path.parent_path()){if(path==root)return true;if(path==path.root_path())break;}return false;}
void outside_checkout(const fs::path& path){
    if(inside(path,fs::weakly_canonical(KEYHUNT_SOURCE_ROOT)))throw std::invalid_argument("runtime state must be outside the source checkout");
    for(auto p=path;!p.empty();p=p.parent_path()){
        if(fs::exists(p/".git"))throw std::invalid_argument("runtime state must be outside every Git checkout");
        if(p==p.root_path())break;
    }
}
fs::path prepare(const std::string& requested){
    const auto path=state_path(requested);
    // Create private directories without changing the process-wide umask or
    // chmod-ing an existing user directory. Existing final directories must be private.
    fs::path current=path.root_path();
    for(const auto& part:path.relative_path()){
        current/=part;
        if(::mkdir(current.c_str(),0700)!=0 && errno!=EEXIST)throw std::runtime_error("cannot create state directory: "+std::string(std::strerror(errno)));
        if(!fs::is_directory(current))throw std::runtime_error("state path contains a non-directory");
    }
    struct stat st{};
    if(::lstat(path.c_str(),&st)!=0 || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077))
        throw std::runtime_error("state directory must be owned by this user with mode 0700");
    outside_checkout(fs::canonical(path));
    return path;
}
void sync_directory(const fs::path& path){
    int fd=::open(path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)throw std::runtime_error("cannot open state directory for sync");
    const int result=::fsync(fd);::close(fd);if(result)throw std::runtime_error("cannot sync state directory");
}
void reserve_file(const fs::path& file,bool exclusive){
    int fd=::open(file.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd>=0){::close(fd);sync_directory(file.parent_path());}
    else if(exclusive || errno!=EEXIST)throw std::runtime_error("cannot exclusively create database: "+std::string(std::strerror(errno)));
    struct stat st{};
    if(::lstat(file.c_str(),&st)!=0 || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077) || st.st_nlink!=1)
        throw std::runtime_error("database must be a private regular file, without symlinks or hardlinks");
}
void open(sqlite3** db,const fs::path& file,int flags){
    if(sqlite3_open_v2(file.c_str(),db,flags|SQLITE_OPEN_NOFOLLOW|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK)error(*db,"open database");
    check_result(*db,sqlite3_busy_timeout(*db,5000),"busy timeout");
    sqlite3_extended_result_codes(*db,1);
}
void copy_snapshot(sqlite3* source,const fs::path& destination){
    sqlite3* target=nullptr;
    if(fs::exists(destination))throw std::runtime_error("backup destination already exists");
    const fs::path temporary=destination.string()+".tmp-"+uuid();
    try {
        reserve_file(temporary,true);open(&target,temporary,SQLITE_OPEN_READWRITE);
        if(sqlite3_exec(target,"PRAGMA synchronous=FULL",nullptr,nullptr,nullptr)!=SQLITE_OK)error(target,"backup synchronous mode");
        auto* copy=sqlite3_backup_init(target,"main",source,"main");
        if(!copy)error(target,"initialize backup");
        int result=SQLITE_OK; unsigned retries=0;
        do {
            result=sqlite3_backup_step(copy,128);
            if(result==SQLITE_BUSY || result==SQLITE_LOCKED){if(++retries>500)break;sqlite3_sleep(10);}
        }while(result==SQLITE_OK || result==SQLITE_BUSY || result==SQLITE_LOCKED);
        const int finish=sqlite3_backup_finish(copy);
        if(result!=SQLITE_DONE || finish!=SQLITE_OK)error(target,"copy backup");
        // A snapshot cannot issue grants if opened accidentally as live state.
        // Restoration rotates the epoch again; C15 supplies reconciled activation.
        char* message=nullptr;
        if(sqlite3_exec(target,"BEGIN IMMEDIATE",nullptr,nullptr,&message)!=SQLITE_OK){sqlite3_free(message);error(target,"seal backup");}
        {Statement s(target,"UPDATE metadata SET value=? WHERE key='quarantine'");s.bind(1,Bytes{1});s.step();}
        {Statement s(target,"UPDATE metadata SET value=? WHERE key='epoch'");s.bind(1,random_bytes(16));s.step();}
        if(sqlite3_exec(target,"COMMIT",nullptr,nullptr,&message)!=SQLITE_OK){sqlite3_free(message);error(target,"seal backup commit");}
        // A published snapshot is one self-contained file. Explicitly drain
        // any copied WAL mode before publication; close alone cannot certify a
        // successful checkpoint after an I/O failure.
        {Statement mode(target,"PRAGMA journal_mode=DELETE");if(!mode.step() || mode.text(0)!="delete")throw std::runtime_error("backup still depends on WAL");}
        check_result(target,sqlite3_close(target),"close backup");target=nullptr;
        int fd=::open(temporary.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0)throw std::runtime_error("open backup for sync failed");
        const int synced=::fsync(fd);::close(fd);if(synced)throw std::runtime_error("backup sync failed");
        // Publish only the sealed, synced snapshot. An existing backup is never
        // overwritten, and a crash before this link leaves no usable destination.
        if(::link(temporary.c_str(),destination.c_str())!=0)throw std::runtime_error("cannot publish backup exclusively");
        if(::unlink(temporary.c_str())!=0)throw std::runtime_error("cannot remove backup staging link");
        sync_directory(destination.parent_path());
    }catch(...){if(target)sqlite3_close(target);std::error_code ignored;fs::remove(temporary,ignored);throw;}
}
}
Bytes digest(Bytes data){alignas(uint64_t) uint8_t result[32];sha256(data.data(),data.size(),result);return Bytes(result,result+32);}
Bytes random_bytes(size_t count){Bytes bytes(count);size_t done=0;while(done<count){ssize_t n=getrandom(bytes.data()+done,count-done,0);if(n<0 && errno==EINTR)continue;if(n<=0)throw std::runtime_error("OS randomness unavailable");done+=size_t(n);}return bytes;}
std::string uuid(){auto b=random_bytes(16);b[6]=(b[6]&15)|64;b[8]=(b[8]&63)|128;std::string s;const char* h="0123456789abcdef";for(size_t i=0;i<16;++i){if(i==4||i==6||i==8||i==10)s+='-';s+=h[b[i]>>4];s+=h[b[i]&15];}return s;}
fs::path state_path(const std::string& explicit_directory){
    std::string value=explicit_directory;
    if(value.empty()){
        const char* override_dir=std::getenv("KEYHUNT_STATE_DIR");const char* xdg=std::getenv("XDG_STATE_HOME");const char* home=std::getenv("HOME");
        if(override_dir && *override_dir)value=override_dir;
        else if(xdg && *xdg)value=std::string(xdg)+"/keyhunt";
        else if(home && *home)value=std::string(home)+"/.local/state/keyhunt";
        else throw std::invalid_argument("set --state-dir or an absolute user state directory");
    }
    if(value.find('\0')!=std::string::npos || !fs::path(value).is_absolute())throw std::invalid_argument("state directory must be absolute");
    auto path=fs::weakly_canonical(value);outside_checkout(path);return path;
}
Statement::Statement(sqlite3* db,const std::string& sql):db_(db){check_result(db_,sqlite3_prepare_v2(db,sql.c_str(),-1,&stmt_,nullptr),"prepare SQL");}
Statement::~Statement(){sqlite3_finalize(stmt_);}
void Statement::bind(int i,const std::string& v){check_result(db_,sqlite3_bind_text(stmt_,i,v.data(),int(v.size()),SQLITE_TRANSIENT),"bind text");}
void Statement::bind(int i,const Bytes& v){check_result(db_,sqlite3_bind_blob(stmt_,i,v.empty()?"":static_cast<const void*>(v.data()),int(v.size()),SQLITE_TRANSIENT),"bind blob");}
void Statement::bind(int i,const core::UInt256& v){auto b=v.bytes();bind(i,Bytes(b.begin(),b.end()));}
void Statement::bind(int i,int64_t v){check_result(db_,sqlite3_bind_int64(stmt_,i,v),"bind integer");}
bool Statement::step(){int result=sqlite3_step(stmt_);if(result==SQLITE_ROW)return true;if(result==SQLITE_DONE)return false;error(db_,"execute SQL");}
Bytes Statement::blob(int i)const{if(sqlite3_column_type(stmt_,i)!=SQLITE_BLOB)throw std::runtime_error("expected BLOB column");int n=sqlite3_column_bytes(stmt_,i);const auto* p=static_cast<const uint8_t*>(sqlite3_column_blob(stmt_,i));return n?Bytes(p,p+n):Bytes{};}
std::string Statement::text(int i)const{if(sqlite3_column_type(stmt_,i)!=SQLITE_TEXT)throw std::runtime_error("expected TEXT column");auto* p=sqlite3_column_text(stmt_,i);return std::string(reinterpret_cast<const char*>(p),size_t(sqlite3_column_bytes(stmt_,i)));}
int64_t Statement::integer(int i)const{if(sqlite3_column_type(stmt_,i)!=SQLITE_INTEGER)throw std::runtime_error("expected INTEGER column");return sqlite3_column_int64(stmt_,i);}
core::UInt256 Statement::wide(int i)const{auto b=blob(i);if(b.size()!=32)throw std::runtime_error("invalid wide integer encoding");core::UInt256::Bytes a{};std::copy(b.begin(),b.end(),a.begin());return core::UInt256::from_bytes(a);}
Database::Database(const std::string& directory){
    if(sqlite3_libversion_number()<3051003)throw std::runtime_error("journal requires SQLite >= 3.51.3 (WAL-reset fix)");
    directory_=prepare(directory);const auto file=directory_/"progress.sqlite";
    try {
        reserve_file(file,false);open(&db_,file,SQLITE_OPEN_READWRITE);
        exec("PRAGMA foreign_keys=ON; PRAGMA trusted_schema=OFF; PRAGMA synchronous=FULL;");
        {
            Transaction tx(*this);
            const auto version=scalar(db_,"PRAGMA user_version"),app=scalar(db_,"PRAGMA application_id");
            const Bytes schema(schema_v1,schema_v1+std::strlen(schema_v1));
            if(version==0 && app==0 && scalar(db_,"SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'")==0){
                exec(schema_v1);
                Statement m(db_,"INSERT INTO migrations VALUES(1,?)");m.bind(1,digest(schema));m.step();
                metadata("epoch",random_bytes(16));metadata("quarantine",Bytes{0});
                exec("PRAGMA application_id="+std::to_string(application_id)+"; PRAGMA user_version=1;");
            }else{
                if(version!=1 || app!=application_id)throw std::runtime_error("foreign or unsupported journal schema");
                Statement m(db_,"SELECT digest FROM migrations WHERE version=1");
                if(!m.step() || m.blob(0)!=digest(schema))throw std::runtime_error("journal migration checksum mismatch");
                if(metadata("epoch").size()!=16 || (metadata("quarantine")!=Bytes{0} && metadata("quarantine")!=Bytes{1}))throw std::runtime_error("invalid journal metadata");
            }
            tx.commit();
        }
        Statement mode(db_,"PRAGMA journal_mode=WAL");if(!mode.step() || mode.text(0)!="wal")throw std::runtime_error("journal requires WAL support");
        if(scalar(db_,"PRAGMA foreign_keys")!=1 || scalar(db_,"PRAGMA synchronous")!=2)throw std::runtime_error("journal durability settings rejected");
    }catch(...){if(db_)sqlite3_close(db_);db_=nullptr;throw;}
}
Database::~Database(){if(db_)sqlite3_close(db_);}
void Database::exec(const std::string& sql){char* message=nullptr;int result=sqlite3_exec(db_,sql.c_str(),nullptr,nullptr,&message);std::string text=message?message:"";sqlite3_free(message);if(result!=SQLITE_OK)throw std::runtime_error("SQLite: "+text);}
Bytes Database::metadata(const std::string& key)const{Statement s(db_,"SELECT value FROM metadata WHERE key=?");s.bind(1,key);if(!s.step())throw std::runtime_error("missing journal metadata");return s.blob(0);}
void Database::metadata(const std::string& key,const Bytes& value){Statement s(db_,"INSERT INTO metadata VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");s.bind(1,key);s.bind(2,value);s.step();}
void Database::writable()const{if(metadata("quarantine")!=Bytes{0})throw std::runtime_error("restored/backup journal is quarantined; allocation and progress disabled");}
void Database::check()const{Statement s(db_,"PRAGMA integrity_check");if(!s.step() || s.text(0)!="ok")throw std::runtime_error("SQLite integrity check failed");Statement fk(db_,"PRAGMA foreign_key_check");if(fk.step())throw std::runtime_error("journal foreign key check failed");}
void Database::backup(const std::string& destination)const{const auto dir=prepare(destination);if(dir==directory_)throw std::invalid_argument("backup destination equals live state");copy_snapshot(db_,dir/"progress.sqlite");}
void Database::restore(const std::string& source,const std::string& destination){
    // Opening the source validates version, checksum and referential integrity;
    // its backup is a new private file and always remains allocation-quarantined.
    const auto file=state_path(source)/"progress.sqlite";
    sqlite3* check=nullptr;
    try {
        open(&check,file,SQLITE_OPEN_READONLY);
        if(scalar(check,"PRAGMA application_id")!=application_id || scalar(check,"PRAGMA user_version")!=1)
            throw std::runtime_error("restore source is not a supported journal");
        sqlite3_close(check);check=nullptr;
    }catch(...){if(check)sqlite3_close(check);throw;}
    Database snapshot(source);snapshot.check();snapshot.backup(destination);
}
Transaction::Transaction(Database& db,bool write):db_(db){db_.exec(write?"BEGIN IMMEDIATE":"BEGIN");}
Transaction::~Transaction(){if(active_)try{db_.exec("ROLLBACK");}catch(...) {}}
void Transaction::commit(){
#ifdef KEYHUNT_TEST_STORAGE_FAILURES
    if(transaction_test_hook)transaction_test_hook("before_commit");
#endif
    db_.exec("COMMIT");active_=false;
#ifdef KEYHUNT_TEST_STORAGE_FAILURES
    if(transaction_test_hook)transaction_test_hook("after_commit");
#endif
}
} // namespace keyhunt::storage::detail
