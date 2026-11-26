// Turns the command line into a command. No hardware access.
#ifndef CLI_H
#define CLI_H

#include <stddef.h>
#include <stdint.h>

#include "controls.h"

enum verb { VERB_HELP, VERB_LIST, VERB_GET, VERB_SET, VERB_CHG, VERB_CAPS, VERB_SCAN, VERB_INFO, VERB_TIMING, VERB_SETUP };

struct command {
	enum verb verb;
	int force;                     // --force was given
	int offline;                   // --offline was given: setup does not ask the monitor database
	uint8_t code;                  // the VCP code for get, set and chg
	const struct control *control; // the named control, or NULL when a raw code was given
	uint16_t value;                // the value for set
	int delta;                     // the signed change for chg
};

// Fills `cmd` from the arguments after the program name.
// Returns 0 on success. On a mistake it returns -1 and writes a one-line explanation to `err`.
int cli_parse(int argc, char **argv, struct command *cmd, char *err, size_t errlen);

// The text printed by `monitorctl help`.
const char *cli_usage(void);

#endif
