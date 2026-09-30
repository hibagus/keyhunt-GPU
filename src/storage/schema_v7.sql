-- C22 binds each portable response to one durable export attempt. A machine
-- request can survive several delivery attempts, but only the current attempt
-- may import grants. Accepted digests retain exact duplicate-import semantics.
CREATE TABLE worker_file_transfers (
    transfer TEXT PRIMARY KEY,
    request TEXT NOT NULL REFERENCES worker_requests(request),
    document TEXT NOT NULL,
    export_boot TEXT NOT NULL,
    exported_at INTEGER NOT NULL CHECK(exported_at>=0),
    state TEXT NOT NULL CHECK(state IN ('pending','superseded','accepted','denied')),
    response_digest BLOB NOT NULL CHECK(length(response_digest) IN (0,32)),
    CHECK ((state IN ('accepted','denied')) = (length(response_digest)=32))
) STRICT;
CREATE UNIQUE INDEX one_pending_file_transfer ON worker_file_transfers(state) WHERE state='pending';
