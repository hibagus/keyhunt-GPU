# C14 signal-thread review finding

HIP may create helper threads before the local control owner installs its
handlers. Blocking signals on the owner thread cannot protect a plain
read-then-clear flag from a handler delivered on another thread: that handler's
new request can be overwritten by the clear. C14 uses compile-time-verified
lock-free atomic flags and an atomic exchange when consuming a request.
Concurrent stop requests use an exchange as well. The handler still performs
only signal-safe flag operations; database, GPU work and force-exit teardown
remain in the owner loop. The process fixture explicitly raises pause, resume
and terminate on helper threads and verifies both forced checkpoints.
