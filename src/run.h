// Carries out a command against a monitor through a transport.
// The transport is a set of function pointers, so tests run against a pretend monitor.
#ifndef RUN_H
#define RUN_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cli.h"

// A Thunderbolt device plugged into the computer. A Thunderbolt monitor is one of these.
struct attached_device {
	char vendor[48];
	char model[48];
	char firmware[16]; // or "" when unknown
};

// Facts about the display that the computer knows without asking the monitor.
struct display_info {
	char model[64];         // the display's product name, or "" when unknown
	unsigned width, height; // the current resolution in pixels, or 0 when unknown
	double refresh;         // the refresh rate in Hz, or 0 when unknown
	char thunderbolt[32];   // the firmware version of the monitor's Thunderbolt controller,
	                        // or "" when the monitor is not connected over Thunderbolt
	unsigned vendor;        // the manufacturer number from the display's EDID, or 0 when unknown
	unsigned product;       // the product number from the display's EDID
	unsigned year, week;    // when the display was built, or 0 when unknown
	char computer[32];      // the computer's model, such as "Mac16,7", or ""
	char system[32];        // the operating system's version, or ""
	unsigned displays;      // how many external displays the computer sees
	unsigned channels;      // how many external displays the computer offers a DDC/CI channel for
	struct attached_device devices[4]; // the Thunderbolt devices plugged in
	int device_count;
};

struct transport {
	void *ctx;
	// Each returns 0 on success.
	int (*write)(void *ctx, const uint8_t *packet, size_t n);
	int (*read)(void *ctx, uint8_t *buffer, size_t n);
	void (*pause)(void *ctx, unsigned milliseconds);
	int (*info)(void *ctx, struct display_info *info);
	// Stores a profile's text under the monitor's identifier and writes where it went to `path`.
	// A problem report is stored the same way, under the identifier and "-problem".
	int (*save)(void *ctx, const char *id, const char *text, char *path, size_t size);
	// Asks the monitor database for the record of the monitor `id`. Returns 0 with the record in
	// `text`, 1 when the database has no reports for it, and -1 when it could not be reached.
	int (*lookup)(void *ctx, const char *id, char *text, size_t size);
	// Asks the person at the keyboard a yes-or-no question. Returns 1 for yes, 0 for no, and -1
	// when there is nobody to ask, because the program is not running in a terminal.
	int (*ask)(void *ctx, const char *question);
	// Sends a profile or a problem report to the monitor database. Returns 0 when the database
	// took it, 1 when it already had this report, -2 when it refused the text, and -1 when it
	// could not be reached or is not taking reports.
	int (*upload)(void *ctx, const char *text);
	// The operating system's code for the last write or read: 0 for success, and otherwise a
	// number that says why it failed. May be NULL when the transport has no such code.
	unsigned (*status)(void *ctx);
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
