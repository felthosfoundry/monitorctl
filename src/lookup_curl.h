// Asks the monitor database at felthosfoundry.com for one monitor's record.
#ifndef LOOKUP_CURL_H
#define LOOKUP_CURL_H

#include <stddef.h>

// Fits the transport's `lookup` slot. `ctx` is unused.
// Returns 0 with the record in `text`, 1 when the database has no reports for `id`, and -1 when
// the database could not be reached or sent too much.
int lookup_curl(void *ctx, const char *id, char *text, size_t size);

#endif
