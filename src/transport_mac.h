#ifndef TRANSPORT_MAC_H
#define TRANSPORT_MAC_H

#include "run.h"

// Connects to the first external display. Returns 0 on success and -1 when there is none.
int transport_mac_open(struct transport *t);

#endif
