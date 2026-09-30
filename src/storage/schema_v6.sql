-- A device slot survives process death. Its OS lock proves when takeover is safe.
-- The coordinator device is only a preferred queue; local active ownership lives here.
CREATE TABLE worker_dispatch (
    device TEXT PRIMARY KEY,
    uuid TEXT NOT NULL,
    project TEXT,
    job BLOB,
    block BLOB,
    CHECK ((project IS NULL AND job IS NULL AND block IS NULL) OR
           (project IS NOT NULL AND length(job)=32 AND length(block)=32)),
    UNIQUE(project,job,block)
) STRICT;
