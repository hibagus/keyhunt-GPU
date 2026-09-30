-- Worker state is local-only. The server uses the same migration history but
-- never populates these tables. Secrets remain in private credential files.
CREATE TABLE worker_settings(
 singleton INTEGER PRIMARY KEY CHECK(singleton=1), instance TEXT NOT NULL,
 configuration TEXT NOT NULL, client TEXT NOT NULL, epoch BLOB NOT NULL,
 last_ack INTEGER NOT NULL, schedule_boot TEXT NOT NULL, next_sync INTEGER NOT NULL,
 outbox_bytes INTEGER NOT NULL CHECK(outbox_bytes>=0),
 outbox_limit INTEGER NOT NULL CHECK(outbox_limit BETWEEN 1048576 AND 1073741824)
);
CREATE TABLE worker_jobs(
 project TEXT NOT NULL, job BLOB NOT NULL,
 PRIMARY KEY(project,job), FOREIGN KEY(project,job) REFERENCES jobs(project,job)
) WITHOUT ROWID;
CREATE TABLE worker_grants(
 project TEXT NOT NULL, job BLOB NOT NULL, block BLOB NOT NULL,
 generation INTEGER NOT NULL CHECK(generation>0), remote TEXT NOT NULL,
 device TEXT NOT NULL, boot TEXT NOT NULL, deadline INTEGER NOT NULL,
 paused INTEGER NOT NULL CHECK(paused IN (0,1)), acknowledged INTEGER NOT NULL CHECK(acknowledged IN (0,1)),
 PRIMARY KEY(project,job,block), FOREIGN KEY(project,job) REFERENCES worker_jobs(project,job)
) WITHOUT ROWID;
CREATE TABLE worker_outbox(
 id INTEGER PRIMARY KEY AUTOINCREMENT,
 project TEXT NOT NULL, job BLOB NOT NULL, block BLOB NOT NULL,
 generation INTEGER NOT NULL, payload BLOB NOT NULL, checksum BLOB NOT NULL CHECK(length(checksum)=32),
 FOREIGN KEY(project,job,block) REFERENCES worker_grants(project,job,block)
);
CREATE TABLE worker_requests(
 request TEXT PRIMARY KEY, body TEXT NOT NULL, through_id INTEGER NOT NULL,
 sent_boot TEXT NOT NULL, sent_at INTEGER NOT NULL, response TEXT NOT NULL,
 acknowledged INTEGER NOT NULL CHECK(acknowledged IN (0,1))
) WITHOUT ROWID;
CREATE UNIQUE INDEX one_pending_machine_request ON worker_requests(acknowledged) WHERE acknowledged=0;
