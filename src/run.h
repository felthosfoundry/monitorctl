// Carries out a command against a monitor through a transport.
// The transport is a set of function pointers, so tests run against a pretend monitor.
#ifndef RUN_H
#define RUN_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cli.h"

// Facts about the display that the computer knows without asking the monitor.
struct display_info {
	char model[64];         // the display's product name, or "" when unknown
	unsigned width, height; // the current resolution in pixels, or 0 when unknown
	double refresh;         // the refresh rate in Hz, or 0 when unknown
	char thunderbolt[32];   // the firmware version of the monitor's Thunderbolt controller,
	                        // or "" when the monitor is not connected over Thunderbolt
};

struct transport {
	void *ctx;
	// Each returns 0 on success.
	int (*write)(void *ctx, const uint8_t *packet, size_t n);
	int (*read)(void *ctx, uint8_t *buffer, size_t n);
	void (*pause)(void *ctx, unsigned milliseconds);
	int (*info)(void *ctx, struct display_info *info);
};

// Exit codes.
#define EXIT_OK 0
#define EXIT_FAILED 1      // the monitor did not answer, or does not support the code
#define EXIT_USAGE 2       // the command line was wrong
#define EXIT_NOT_APPLIED 3 // the monitor accepted a write and the value did not change
#define EXIT_REFUSED 4     // the write is guarded and --force was not given

// Results go to `out`, and explanations of anything that went wrong go to `err`.
int run_command(const struct command *cmd, const struct transport *transport, FILE *out, FILE *err);

#endif
