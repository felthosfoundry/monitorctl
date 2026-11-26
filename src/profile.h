// A monitor profile: what one monitor supports, as `monitorctl setup` found it.
// The profile is a plain text file with one fact per line, so it can be read without a parser
// library. It holds no serial number.
#ifndef PROFILE_H
#define PROFILE_H

#include <stddef.h>
#include <stdint.h>

// What happened when setup tested a write to a code.
enum write_result {
	WRITE_UNTESTED, // setup did not write to this code
	WRITE_WORKS,    // the monitor applied the write
	WRITE_IGNORED,  // the monitor accepted the write and the value did not change
};

struct profile_code {
	uint8_t code;
	uint16_t maximum;
	enum write_result write;
};

struct profile {
	char id[8];                     // the monitor's identifier, such as "SAM0F1E"
	char model[64];                 // the product name, or "" when unknown
	unsigned year, week;            // when the monitor was built, or 0 when unknown
	char thunderbolt[32];           // the Thunderbolt firmware version, or ""
	char caps[4096];                // the monitor's capability string, or ""
	struct profile_code codes[256]; // every code the monitor supports, in code order
	int count;
};

// Builds a monitor's identifier from the two numbers in its EDID: three manufacturer letters and
// the product number as four hexadecimal digits. Samsung's 19501 and 3870 give "SAM0F1E".
// Returns 0 on success, and -1 when the numbers are not a valid manufacturer and product.
int profile_id(unsigned vendor, unsigned product, char id[8]);

// Writes the profile as text. Returns the length, or -1 when `size` is too small.
int profile_format(const struct profile *profile, char *text, size_t size);

// Saves a profile's text as <id>.txt in the monitorctl folder under XDG_CONFIG_HOME, or under
// .config in the home folder. Writes the file's path to `path`. Returns 0 on success.
// `ctx` is unused. It is there so this function fits the transport's `save` slot.
int profile_save(void *ctx, const char *id, const char *text, char *path, size_t size);

#endif
