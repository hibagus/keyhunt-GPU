#include "sqlite.h"
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
using namespace keyhunt::storage::detail;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F>void rejects(F fn){try{fn();}catch(const std::exception&){return;}throw std::runtime_error("expected rejection");}
int main(){
 try{
  // Honor TMPDIR so journal fixtures can live outside a managed checkout.
        std::string temporary=(std::filesystem::temp_directory_path()/"keyhunt-c12-db-XXXXXX").string();require(mkdtemp(temporary.data()),"mkdtemp");std::filesystem::path root=temporary;
  struct Cleanup{std::filesystem::path p;~Cleanup(){std::filesystem::remove_all(p);}}cleanup{root};
  rejects([]{Database d("relative");});rejects([]{Database d(std::filesystem::current_path().string());});
  std::filesystem::create_directory(root/"repo");std::filesystem::create_directory(root/"repo/.git");
  rejects([&]{Database d((root/"repo/state").string());});
  std::filesystem::create_directory_symlink(root/"repo",root/"alias");rejects([&]{Database d((root/"alias/state").string());});
  std::filesystem::create_directory(root/"public");chmod((root/"public").c_str(),0755);rejects([&]{Database d((root/"public").string());});
  Database db((root/"live").string());db.check();db.writable();
  const auto epoch=db.metadata("epoch");require(epoch.size()==16,"epoch");
  {Transaction tx(db);db.exec("INSERT INTO projects VALUES('aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa','rollback')");}
  {Statement s(db.handle(),"SELECT count(*) FROM projects");require(s.step()&&s.integer(0)==0,"rollback");}
  {Transaction tx(db);db.exec("INSERT INTO projects VALUES('aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa','retained')");tx.commit();}
  // Releasing an inner savepoint does not survive outer rollback.
  {Transaction outer(db);{Transaction inner(db);db.exec("INSERT INTO projects VALUES('bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb','nested')");inner.commit();}}
  {Statement s(db.handle(),"SELECT count(*) FROM projects");require(s.step()&&s.integer(0)==1,"inner transaction escaped rollback");}
  {Transaction outer(db);{Transaction inner(db);db.exec("DELETE FROM projects");}outer.commit();}
  {Statement s(db.handle(),"SELECT count(*) FROM projects");require(s.step()&&s.integer(0)==1,"savepoint rollback lost prior rows");}
  db.backup((root/"backup").string());rejects([&]{db.backup((root/"backup").string());});
  sqlite3* raw=nullptr;require(sqlite3_open_v2((root/"backup/progress.sqlite").c_str(),&raw,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK,"open snapshot");
  {Statement mode(raw,"PRAGMA journal_mode");require(mode.step()&&mode.text(0)=="delete","snapshot needs WAL");}sqlite3_close(raw);
  Database backup((root/"backup").string());backup.check();rejects([&]{backup.writable();});require(backup.metadata("epoch")!=epoch,"backup epoch");
  Database::restore((root/"backup").string(),(root/"restored").string());
  Database restored((root/"restored").string());restored.check();rejects([&]{restored.writable();});
  {Statement s(restored.handle(),"SELECT name FROM projects");require(s.step()&&s.text(0)=="retained","snapshot missing commit");}
  require(restored.metadata("epoch")!=backup.metadata("epoch"),"restore epoch");
  {Database future((root/"future").string());future.exec("PRAGMA user_version=99");}rejects([&]{Database future((root/"future").string());});
  {Database bad((root/"bad").string());bad.exec("UPDATE migrations SET digest=zeroblob(32)");}rejects([&]{Database bad((root/"bad").string());});
  struct stat st{};stat((root/"live/progress.sqlite").c_str(),&st);require((st.st_mode&0777)==0600,"database mode");
  std::cout<<"Storage schema, private paths, transactions and quarantined backup/restore passed (SQLite "<<sqlite3_libversion()<<")\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
