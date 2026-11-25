#ifndef INFO_MAC_H
#define INFO_MAC_H

#include "run.h"

// Describes the first external display from what macOS knows about it.
// Returns 0 on success and -1 when macOS lists no external display.
int info_mac_read(void *ctx, struct display_info *info);

#endif
