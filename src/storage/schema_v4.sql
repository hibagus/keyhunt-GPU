-- C15 machine requests retain their exact committed response. Never recompute
-- a retry: doing so could extend leases or allocate another set of blocks.
CREATE TABLE coordinator_syncs(
 client TEXT NOT NULL, instance TEXT NOT NULL, request TEXT NOT NULL,
 payload BLOB NOT NULL CHECK(length(payload)=32), response TEXT NOT NULL,
 epoch BLOB NOT NULL CHECK(length(epoch)=16), created INTEGER NOT NULL,
 PRIMARY KEY(client,instance,request),
 FOREIGN KEY(client) REFERENCES coordinator_clients(client)
) WITHOUT ROWID;
CREATE TABLE coordinator_devices(
 project TEXT NOT NULL, job BLOB NOT NULL, block BLOB NOT NULL,
 client TEXT NOT NULL, instance TEXT NOT NULL, device TEXT NOT NULL,
 PRIMARY KEY(project,job,block),
 FOREIGN KEY(project,job,block) REFERENCES assignments(project,job,block) ON DELETE CASCADE,
 FOREIGN KEY(client) REFERENCES coordinator_clients(client)
) WITHOUT ROWID;
CREATE INDEX machine_devices ON coordinator_devices(client,instance,device);
