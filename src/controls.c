#include "controls.h"

#include <stdlib.h>
#include <string.h>

static const struct value_name MUTE[] = {{1, "on"}, {2, "off"}, {0, NULL}};
static const struct value_name POWER[] = {{1, "on"}, {2, "standby"}, {3, "suspend"}, {4, "off"}, {0, NULL}};
static const struct value_name INPUT[] = {{15, "dp1"}, {16, "dp2"}, {17, "hdmi1"}, {18, "hdmi2"}, {27, "usbc"}, {0, NULL}};
static const struct value_name COLOR_PRESET[] = {
	{1, "srgb"},   {2, "native"}, {3, "4000k"},   {4, "5000k"},   {5, "6500k"},  {6, "7500k"}, {7, "8200k"},
	{8, "9300k"},  {9, "10000k"}, {10, "11500k"}, {11, "user1"},  {12, "user2"}, {13, "user3"}, {0, NULL},
};

// Codes are from the VESA Monitor Control Command Set. A monitor supports only some of them.
static const struct control CONTROLS[] = {
	{"brightness", 0x10, "Backlight brightness.", NULL},
	{"contrast", 0x12, "Contrast.", NULL},
	{"volume", 0x62, "Speaker or headphone volume.", NULL},
	{"mute", 0x8D, "Sound mute: on or off.", MUTE},
	{"red", 0x16, "Red gain.", NULL},
	{"green", 0x18, "Green gain.", NULL},
	{"blue", 0x1A, "Blue gain.", NULL},
	{"color-preset", 0x14, "Colour temperature preset, such as 6500k or srgb.", COLOR_PRESET},
	{"picture-mode", 0xDC, "Picture mode. The meaning of each number depends on the monitor.", NULL},
	{"input", 0x60, "Input source. Some monitors report it and ignore changes.", INPUT},
	{"power", 0xD6, "Power mode: on or off. Off needs --force.", POWER},
};
#define COUNT (sizeof CONTROLS / sizeof CONTROLS[0])

const struct control *controls_all(size_t *count) {
	*count = COUNT;
	return CONTROLS;
}

const struct control *control_find(const char *name) {
	for (size_t i = 0; i < COUNT; i++)
		if (strcmp(CONTROLS[i].name, name) == 0) return &CONTROLS[i];
	return NULL;
}

const struct control *control_for_code(uint8_t code) {
	for (size_t i = 0; i < COUNT; i++)
		if (CONTROLS[i].code == code) return &CONTROLS[i];
	return NULL;
}

// Reads a whole number from 0 to `limit`, written as decimal or 0x-prefixed hex. Returns 0 on success.
static int parse_number(const char *text, unsigned long limit, unsigned long *number) {
	if (text[0] == '\0' || text[0] == '-' || text[0] == '+' || text[0] == ' ') return -1;
	int hex = text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
	if (hex && text[2] == '\0') return -1;
	char *end = NULL;
	unsigned long parsed = strtoul(text, &end, hex ? 16 : 10);
	if (*end != '\0' || parsed > limit) return -1;
	*number = parsed;
	return 0;
}

int parse_code(const char *text, uint8_t *code) {
	unsigned long number;
	if (parse_number(text, 0xFF, &number) != 0) return -1;
	*code = (uint8_t)number;
	return 0;
}

int control_parse_value(const struct control *control, const char *text, uint16_t *value) {
	if (control && control->names) {
		for (const struct value_name *n = control->names; n->name; n++) {
			if (strcmp(n->name, text) == 0) {
				*value = n->value;
				return 0;
			}
		}
	}
	unsigned long number;
	if (parse_number(text, 0xFFFF, &number) != 0) return -1;
	*value = (uint16_t)number;
	return 0;
}

const char *control_value_name(const struct control *control, uint16_t value) {
	if (!control || !control->names) return NULL;
	for (const struct value_name *n = control->names; n->name; n++)
		if (n->value == value) return n->name;
	return NULL;
}

const char *guard_reason(uint8_t code, uint16_t value) {
	if (code == 0xD6 && value != 1) {
		return "this switches the monitor off. A monitor that is off stops listening for commands, "
		       "so only the button or joystick on the monitor can switch it on again.";
	}
	if (code == 0x04 || code == 0x05 || code == 0x06 || code == 0x08 || code == 0x0A) {
		return "this code restores factory settings and discards the monitor's current ones.";
	}
	if (code >= 0xE0) {
		return "this is a manufacturer-private code, and its effect on this monitor is unknown.";
	}
	return NULL;
}
