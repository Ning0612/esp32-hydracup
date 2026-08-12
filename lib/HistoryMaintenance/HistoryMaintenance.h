#pragma once

// Clearing the drink history is deferred to the next boot on purpose.
//
// Doing it while the system runs means racing every producer that writes the state being
// wiped: the event logger's queue (including a message already dequeued and waiting on the
// filesystem lock), the counter store's queued and retrying saves, an NVS restore still
// reading pre-wipe values, and a cloud batch read under the lock but posted outside it.
// Each of those needs its own barrier, and the file wipe and the counter wipe still cannot
// be made atomic with respect to a drink happening between them.
//
// At boot none of it exists yet: no tasks have been created and the partitions are not
// mounted, so the wipe is a straight line with nothing to serialise against. The directories
// are recreated by EventLogger and CloudSyncClient during their normal init later in the
// same boot, so nothing has to put them back by hand.

// Records the request so the next boot performs it. Returns false if it could not be stored,
// in which case nothing was scheduled and the caller should report failure.
bool history_request_clear();

// Performs a pending clear. Call once during startup, after NVS is available and before the
// filesystems are mounted. Does nothing when no clear is pending.
void history_apply_pending_clear();
