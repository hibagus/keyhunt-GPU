-- C12 local journal. Every job-owned row carries the complete project/job key.
-- Wide integers are canonical fixed-width big-endian BLOBs: SQLite's BLOB
-- ordering then agrees with unsigned 256-bit ordering, without signed truncation.
CREATE TABLE migrations(version INTEGER PRIMARY KEY, digest BLOB NOT NULL CHECK(typeof(digest)='blob' AND length(digest)=32));
CREATE TABLE metadata(key TEXT PRIMARY KEY, value BLOB NOT NULL) WITHOUT ROWID;
CREATE TABLE projects(project TEXT PRIMARY KEY CHECK(length(project)=36), name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 256)) WITHOUT ROWID;
CREATE TABLE jobs(
 project TEXT NOT NULL, job BLOB NOT NULL CHECK(typeof(job)='blob' AND length(job)=32),
 manifest BLOB NOT NULL, root_begin BLOB NOT NULL CHECK(typeof(root_begin)='blob' AND length(root_begin)=32),
 root_end BLOB NOT NULL CHECK(typeof(root_end)='blob' AND length(root_end)=32 AND root_end>root_begin),
 width BLOB NOT NULL CHECK(typeof(width)='blob' AND length(width)=32),
 block_count BLOB NOT NULL CHECK(typeof(block_count)='blob' AND length(block_count)=32),
 seed BLOB NOT NULL CHECK(typeof(seed)='blob' AND length(seed)=32),
 counter BLOB NOT NULL CHECK(typeof(counter)='blob' AND length(counter)=32),
 next_generation INTEGER NOT NULL CHECK(next_generation>0),
 PRIMARY KEY(project,job), FOREIGN KEY(project) REFERENCES projects(project)
) WITHOUT ROWID;
-- An absent subtree is wholly unexplored. A zero-free node represents a
-- wholly reserved/finished subtree, with redundant descendants removed.
CREATE TABLE free_nodes(
 project TEXT NOT NULL, job BLOB NOT NULL, lo BLOB NOT NULL CHECK(typeof(lo)='blob' AND length(lo)=32),
 depth INTEGER NOT NULL CHECK(depth BETWEEN 0 AND 256),
 free BLOB NOT NULL CHECK(typeof(free)='blob' AND length(free)=32),
 PRIMARY KEY(project,job,lo,depth), FOREIGN KEY(project,job) REFERENCES jobs(project,job)
) WITHOUT ROWID;
CREATE TABLE assignments(
 project TEXT NOT NULL, job BLOB NOT NULL, block BLOB NOT NULL CHECK(typeof(block)='blob' AND length(block)=32),
 owner TEXT NOT NULL CHECK(length(owner) BETWEEN 1 AND 128),
 generation INTEGER NOT NULL CHECK(generation>0), expires INTEGER NOT NULL CHECK(expires>0),
 started INTEGER NOT NULL CHECK(started IN (0,1)),
 PRIMARY KEY(project,job,block), UNIQUE(project,job,generation),
 FOREIGN KEY(project,job) REFERENCES jobs(project,job)
) WITHOUT ROWID;
CREATE TABLE coverage(
 project TEXT NOT NULL, job BLOB NOT NULL, block BLOB NOT NULL,
 begin BLOB NOT NULL CHECK(typeof(begin)='blob' AND length(begin)=32),
 end BLOB NOT NULL CHECK(typeof(end)='blob' AND length(end)=32 AND end>begin),
 PRIMARY KEY(project,job,block,begin),
 FOREIGN KEY(project,job,block) REFERENCES assignments(project,job,block)
) WITHOUT ROWID;
-- Completed block runs replace active rows and partial per-block intervals.
CREATE TABLE finished(
 project TEXT NOT NULL, job BLOB NOT NULL,
 begin BLOB NOT NULL CHECK(typeof(begin)='blob' AND length(begin)=32),
 end BLOB NOT NULL CHECK(typeof(end)='blob' AND length(end)=32 AND end>begin),
 PRIMARY KEY(project,job,begin), FOREIGN KEY(project,job) REFERENCES jobs(project,job)
) WITHOUT ROWID;
CREATE TABLE requests(
 project TEXT NOT NULL, job BLOB NOT NULL, owner TEXT NOT NULL, operation TEXT NOT NULL, request TEXT NOT NULL,
 payload BLOB NOT NULL CHECK(typeof(payload)='blob' AND length(payload)=32), response BLOB NOT NULL,
 PRIMARY KEY(project,job,owner,operation,request), FOREIGN KEY(project,job) REFERENCES jobs(project,job)
) WITHOUT ROWID;
CREATE TABLE events(
 sequence INTEGER PRIMARY KEY AUTOINCREMENT, project TEXT NOT NULL, job BLOB NOT NULL,
 operation TEXT NOT NULL, owner TEXT NOT NULL, request TEXT NOT NULL, created INTEGER NOT NULL,
 FOREIGN KEY(project,job) REFERENCES jobs(project,job)
);
CREATE INDEX scoped_events ON events(project,job,sequence);
