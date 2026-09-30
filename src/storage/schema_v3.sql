-- C15/S02: exact certificate identities and project-scoped authorization.
-- Published migrations are immutable; later protocol/outbox tables get a new version.
CREATE TABLE coordinator_settings(
 singleton INTEGER PRIMARY KEY CHECK(singleton=1),
 bootstrapped INTEGER NOT NULL CHECK(bootstrapped IN (0,1)),
 max_lifetime INTEGER NOT NULL CHECK(max_lifetime BETWEEN 1 AND 2592000),
 last_issued INTEGER NOT NULL CHECK(last_issued>=0)
);
INSERT INTO coordinator_settings VALUES(1,0,2592000,0);
CREATE TABLE coordinator_clients(
 client TEXT PRIMARY KEY CHECK(length(client)=36), name TEXT NOT NULL,
 enabled INTEGER NOT NULL CHECK(enabled IN (0,1))
) WITHOUT ROWID;
CREATE TABLE coordinator_credentials(
 fingerprint BLOB PRIMARY KEY CHECK(length(fingerprint)=32),
 client TEXT NOT NULL, spki BLOB NOT NULL CHECK(length(spki)=32),
 issuer TEXT NOT NULL, serial TEXT NOT NULL,
 not_before INTEGER NOT NULL, not_after INTEGER NOT NULL CHECK(not_after>not_before),
 certificate BLOB NOT NULL, enabled INTEGER NOT NULL CHECK(enabled IN (0,1)),
 FOREIGN KEY(client) REFERENCES coordinator_clients(client)
) WITHOUT ROWID;
CREATE INDEX coordinator_client_credentials ON coordinator_credentials(client);
CREATE TABLE coordinator_memberships(
 project TEXT NOT NULL, client TEXT NOT NULL, role INTEGER NOT NULL CHECK(role BETWEEN 1 AND 3),
 PRIMARY KEY(project,client), FOREIGN KEY(project) REFERENCES projects(project),
 FOREIGN KEY(client) REFERENCES coordinator_clients(client)
) WITHOUT ROWID;
CREATE TABLE coordinator_controls(
 project TEXT NOT NULL, job BLOB NOT NULL, paused INTEGER NOT NULL CHECK(paused IN (0,1)),
 PRIMARY KEY(project,job), FOREIGN KEY(project,job) REFERENCES jobs(project,job)
) WITHOUT ROWID;
CREATE TABLE coordinator_events(
 sequence INTEGER PRIMARY KEY AUTOINCREMENT, created INTEGER NOT NULL,
 actor TEXT NOT NULL, credential BLOB NOT NULL, operation TEXT NOT NULL,
 project TEXT NOT NULL, detail TEXT NOT NULL
);
