// Talks to the monitor database at felthosfoundry.com.
#ifndef DATABASE_CURL_H
#define DATABASE_CURL_H

#include <stddef.h>

// Both functions fit slots in the transport. `ctx` is unused.

// Asks for one monitor's record.
// Returns 0 with the record in `text`, 1 when the database has no reports for `id`, and -1 when
// the database could not be reached or sent too much.
int database_lookup(void *ctx, const char *id, char *text, size_t size);

// Sends one profile.
// Returns 0 when the database took it, 1 when it already had this report from this sender, -2
// when it refused the profile, and -1 when it could not be reached or is not taking reports.
int database_upload(void *ctx, const char *text);

#endif
