-- C13 binds real canonical inputs before accepting verified results/coverage.
-- The C12 migration remains byte-for-byte immutable.
CREATE TABLE search_bindings(
 project TEXT NOT NULL, job BLOB NOT NULL, configuration BLOB NOT NULL,
 targets BLOB NOT NULL, software TEXT NOT NULL,
 next_executor INTEGER NOT NULL CHECK(next_executor>0),
 PRIMARY KEY(project,job), FOREIGN KEY(project,job) REFERENCES jobs(project,job)
) WITHOUT ROWID;
CREATE TABLE executors(
 project TEXT NOT NULL, job BLOB NOT NULL, block BLOB NOT NULL,
 assignment_generation INTEGER NOT NULL CHECK(assignment_generation>0),
 executor_generation INTEGER NOT NULL CHECK(executor_generation>0),
 PRIMARY KEY(project,job,block),
 FOREIGN KEY(project,job,block) REFERENCES assignments(project,job,block) ON DELETE CASCADE,
 FOREIGN KEY(project,job) REFERENCES search_bindings(project,job)
) WITHOUT ROWID;
CREATE TABLE results(
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 project TEXT NOT NULL, job BLOB NOT NULL,
 block BLOB NOT NULL CHECK(typeof(block)='blob' AND length(block)=32),
 scalar BLOB NOT NULL CHECK(typeof(scalar)='blob' AND length(scalar)=32),
 target INTEGER NOT NULL CHECK(target BETWEEN 0 AND 1048575),
 UNIQUE(project,job,scalar,target),
 FOREIGN KEY(project,job) REFERENCES search_bindings(project,job)
);
CREATE INDEX scoped_results ON results(project,job,id);
-- Retain a canonical payload beside the C12 receipt's SHA256. Auditing can
-- reconstruct the accepted union and match set, including partial BSGS results.
CREATE TABLE checkpoints(
 project TEXT NOT NULL, job BLOB NOT NULL, owner TEXT NOT NULL,
 operation TEXT NOT NULL CHECK(operation='checkpoint'), request TEXT NOT NULL,
 payload BLOB NOT NULL,
 PRIMARY KEY(project,job,owner,operation,request),
 FOREIGN KEY(project,job,owner,operation,request)
  REFERENCES requests(project,job,owner,operation,request),
 FOREIGN KEY(project,job) REFERENCES search_bindings(project,job)
) WITHOUT ROWID;
