// The settings monitorctl knows by name, and the codes it refuses to write without --force.
#ifndef CONTROLS_H
#define CONTROLS_H

#include <stddef.h>
#include <stdint.h>

struct value_name {
	uint16_t value;
	const char *name; // NULL marks the end of a list
};

// How `monitorctl setup` treats a control. Setup writes only to the controls marked here.
enum probe {
	PROBE_NONE,   // never written by setup
	PROBE_WATCH,  // not tested, and put back if a test changed it as a side effect
	PROBE_STEP,   // tested by moving it one step, then put back
	PROBE_TOGGLE, // tested by switching between its two values, 1 and 2, then put back
};

struct control {
	const char *name;
	uint8_t code;
	const char *help;
	const struct value_name *names; // NULL when the control is a plain number
	enum probe probe;
};

// Every named control, in the order `monitorctl list` prints them.
const struct control *controls_all(size_t *count);

// Finds a control by name, or by code with control_for_code. Both return NULL when there is none.
const struct control *control_find(const char *name);
const struct control *control_for_code(uint8_t code);

// Reads a VCP code written as decimal or 0x-prefixed hex. Returns 0 on success.
int parse_code(const char *text, uint8_t *code);

// Reads a value for a control: one of its value names, or a number from 0 to 65535 in decimal or
// 0x-prefixed hex. `control` may be NULL for a raw code. Returns 0 on success.
int control_parse_value(const struct control *control, const char *text, uint16_t *value);

// The name of a value, such as "on" for mute 1, or NULL when the value has no name.
const char *control_value_name(const struct control *control, uint16_t value);

// Why writing this value to this code is refused without --force, or NULL when it is allowed.
const char *guard_reason(uint8_t code, uint16_t value);

#endif
