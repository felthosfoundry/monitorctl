// Tests for everything except the code that talks to real hardware.
// Commands run against a pretend monitor, defined below, that answers DDC/CI packets.
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/cli.h"
#include "../src/controls.h"
#include "../src/ddc.h"
#include "../src/run.h"

static int checks, failures;
#define CHECK(cond) \
	do { \
		checks++; \
		if (!(cond)) { \
			failures++; \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)
#define CHECK_STR(actual, expected) \
	do { \
		checks++; \
		if (strcmp((actual), (expected)) != 0) { \
			failures++; \
			fprintf(stderr, "FAIL %s:%d: got \"%s\", expected \"%s\"\n", __FILE__, __LINE__, (actual), (expected)); \
		} \
	} while (0)
#define CHECK_HAS(text, part) \
	do { \
		checks++; \
		if (strstr((text), (part)) == NULL) { \
			failures++; \
			fprintf(stderr, "FAIL %s:%d: \"%s\" does not contain \"%s\"\n", __FILE__, __LINE__, (text), (part)); \
		} \
	} while (0)

// ---------------------------------------------------------------------------------------------
// The pretend monitor

struct setting {
	uint8_t code;
	uint16_t current, maximum;
	int ignores_writes;
};

struct fake {
	struct setting settings[16];
	int count;
	const char *caps;
	uint8_t pending[DDC_MAX_PACKET]; // the last request received
	int garbage_replies;             // how many upcoming replies to corrupt
	int unreachable;                 // the monitor is off: nothing gets through
	int sets;                        // how many set requests arrived
	int bad_packets;                 // requests whose checksum was wrong
	struct display_info info;        // what the computer knows about the display
	int info_fails;                  // the computer cannot describe the display
	struct timing timing;            // what the monitor reports about its signal
	int no_timing;                   // the monitor does not answer timing requests
};

static struct setting *fake_find(struct fake *f, uint8_t code) {
	for (int i = 0; i < f->count; i++)
		if (f->settings[i].code == code) return &f->settings[i];
	return NULL;
}

static uint8_t reply_checksum(const uint8_t *bytes, size_t n) {
	uint8_t sum = 0x50;
	for (size_t i = 0; i < n; i++) sum ^= bytes[i];
	return sum;
}

static int fake_write(void *ctx, const uint8_t *packet, size_t n) {
	struct fake *f = ctx;
	if (f->unreachable) return -1;
	uint8_t sum = 0x6E ^ 0x51;
	for (size_t i = 0; i + 1 < n; i++) sum ^= packet[i];
	if (n < 3 || packet[n - 1] != sum || (size_t)(packet[0] & 0x7F) != n - 2) f->bad_packets++;
	memset(f->pending, 0, sizeof f->pending);
	memcpy(f->pending, packet, n);
	if (packet[1] == 0x03) {
		f->sets++;
		struct setting *s = fake_find(f, packet[2]);
		if (s && !s->ignores_writes) s->current = (uint16_t)(packet[3] << 8 | packet[4]);
		// Like the real monitor: once switched off, it stops listening.
		if (packet[2] == 0xD6 && s && s->current != 1) f->unreachable = 1;
	}
	return 0;
}

static int fake_read(void *ctx, uint8_t *buffer, size_t n) {
	struct fake *f = ctx;
	if (f->unreachable) return -1;
	memset(buffer, 0, n);
	if (f->garbage_replies > 0) {
		f->garbage_replies--;
		return 0;
	}
	if (f->pending[1] == 0x01) {
		struct setting *s = fake_find(f, f->pending[2]);
		uint8_t reply[11] = {0x6E, 0x88, 0x02, s ? 0x00 : 0x01, f->pending[2], 0x00, 0, 0, 0, 0, 0};
		if (s) {
			reply[6] = (uint8_t)(s->maximum >> 8);
			reply[7] = (uint8_t)(s->maximum & 0xFF);
			reply[8] = (uint8_t)(s->current >> 8);
			reply[9] = (uint8_t)(s->current & 0xFF);
		}
		reply[10] = reply_checksum(reply, 10);
		memcpy(buffer, reply, n < sizeof reply ? n : sizeof reply);
	} else if (f->pending[1] == 0x07 && !f->no_timing) {
		uint8_t reply[9] = {0x6E, 0x06, 0x4E, f->timing.status,
			(uint8_t)(f->timing.horizontal >> 8), (uint8_t)(f->timing.horizontal & 0xFF),
			(uint8_t)(f->timing.vertical >> 8), (uint8_t)(f->timing.vertical & 0xFF), 0};
		reply[8] = reply_checksum(reply, 8);
		memcpy(buffer, reply, n < sizeof reply ? n : sizeof reply);
	} else if (f->pending[1] == 0xF3) {
		uint16_t offset = (uint16_t)(f->pending[2] << 8 | f->pending[3]);
		size_t total = strlen(f->caps);
		size_t len = offset >= total ? 0 : total - offset;
		if (len > 32) len = 32;
		uint8_t reply[DDC_MAX_PACKET] = {0x6E, (uint8_t)(0x80 | (3 + len)), 0xE3, f->pending[2], f->pending[3]};
		memcpy(reply + 5, f->caps + offset, len);
		reply[5 + len] = reply_checksum(reply, 5 + len);
		memcpy(buffer, reply, n < 6 + len ? n : 6 + len);
	}
	return 0;
}

static void fake_pause(void *ctx, unsigned milliseconds) {
	(void)ctx;
	(void)milliseconds;
}

static int fake_info(void *ctx, struct display_info *info) {
	struct fake *f = ctx;
	if (f->info_fails) return -1;
	*info = f->info;
	return 0;
}

static struct fake samsung(void) {
	struct fake f = {
		.settings = {
			{0x10, 95, 100, 0},  // brightness
			{0x12, 75, 100, 0},  // contrast
			{0x62, 50, 100, 0},  // volume
			{0x8D, 2, 2, 0},     // mute, off
			{0x14, 4, 11, 0},    // colour preset
			{0x60, 4, 3, 1},     // input: this monitor ignores writes to it
			{0xD6, 1, 4, 0},     // power
			{0xB6, 3, 0, 0},     // a code that reports a maximum of 0
		},
		.count = 8,
		.caps = "(prot(monitor)type(lcd)model(FALCON)cmds(01 02 03)vcp(10 12 14(01 04 05) 60(01 03) 62 8D))",
		.info = {.model = "C34J79x", .width = 3440, .height = 1440, .refresh = 100.0, .thunderbolt = "61.2"},
		.timing = {.status = 0x03, .horizontal = 15090, .vertical = 10040},
	};
	return f;
}

// Runs one command line against the pretend monitor and captures what it printed.
static char out_text[4096], err_text[4096];
static int run(struct fake *f, const char *line) {
	char copy[256];
	char *argv[16];
	int argc = 0;
	strncpy(copy, line, sizeof copy - 1);
	copy[sizeof copy - 1] = '\0';
	for (char *word = strtok(copy, " "); word && argc < 16; word = strtok(NULL, " ")) argv[argc++] = word;

	struct command cmd;
	char message[256];
	out_text[0] = err_text[0] = '\0';
	if (cli_parse(argc, argv, &cmd, message, sizeof message) != 0) {
		snprintf(err_text, sizeof err_text, "%s", message);
		return EXIT_USAGE;
	}
	char *out_buffer = NULL, *err_buffer = NULL;
	size_t out_size = 0, err_size = 0;
	FILE *out = open_memstream(&out_buffer, &out_size);
	FILE *err = open_memstream(&err_buffer, &err_size);
	struct transport t = {f, fake_write, fake_read, fake_pause, fake_info};
	int code = run_command(&cmd, &t, out, err);
	fclose(out);
	fclose(err);
	snprintf(out_text, sizeof out_text, "%s", out_buffer);
	snprintf(err_text, sizeof err_text, "%s", err_buffer);
	free(out_buffer);
	free(err_buffer);
	return code;
}

// ---------------------------------------------------------------------------------------------
// Packets

static void test_packets(void) {
	uint8_t p[DDC_MAX_PACKET];

	// "Read brightness" is the best-known DDC/CI packet: 82 01 10 AC.
	CHECK(ddc_get_request(0x10, p) == 4);
	CHECK(p[0] == 0x82 && p[1] == 0x01 && p[2] == 0x10 && p[3] == 0xAC);

	// "Set brightness to 50": 84 03 10 00 32 9A.
	CHECK(ddc_set_request(0x10, 50, p) == 6);
	CHECK(p[0] == 0x84 && p[1] == 0x03 && p[2] == 0x10 && p[3] == 0x00 && p[4] == 0x32 && p[5] == 0x9A);

	// A value above 255 uses both value bytes.
	ddc_set_request(0x10, 0x1234, p);
	CHECK(p[3] == 0x12 && p[4] == 0x34);

	// "Send the capability string from offset 0x0120".
	CHECK(ddc_caps_request(0x0120, p) == 5);
	CHECK(p[0] == 0x83 && p[1] == 0xF3 && p[2] == 0x01 && p[3] == 0x20);
	CHECK(p[4] == (uint8_t)(0x6E ^ 0x51 ^ 0x83 ^ 0xF3 ^ 0x01 ^ 0x20));

	// "Report the signal timing": 81 07 B9.
	CHECK(ddc_timing_request(p) == 3);
	CHECK(p[0] == 0x81 && p[1] == 0x07 && p[2] == 0xB9);
}

static void test_vcp_replies(void) {
	struct vcp_value v;
	uint8_t ok[11] = {0x6E, 0x88, 0x02, 0x00, 0x10, 0x00, 0x00, 0x64, 0x00, 0x5F, 0};
	ok[10] = reply_checksum(ok, 10);
	CHECK(ddc_parse_vcp_reply(ok, sizeof ok, 0x10, &v) == 0);
	CHECK(v.current == 95 && v.maximum == 100 && v.type == 0);

	// A real reply is read into a larger buffer; the extra zero bytes are harmless.
	uint8_t padded[12] = {0};
	memcpy(padded, ok, sizeof ok);
	CHECK(ddc_parse_vcp_reply(padded, sizeof padded, 0x10, &v) == 0);

	uint8_t unsupported[11] = {0x6E, 0x88, 0x02, 0x01, 0x99, 0, 0, 0, 0, 0, 0};
	unsupported[10] = reply_checksum(unsupported, 10);
	CHECK(ddc_parse_vcp_reply(unsupported, sizeof unsupported, 0x99, &v) == 1);

	// A reply about a different code, a corrupted reply, an empty reply and a short one are all invalid.
	CHECK(ddc_parse_vcp_reply(ok, sizeof ok, 0x12, &v) == -1);
	uint8_t corrupt[11];
	memcpy(corrupt, ok, sizeof ok);
	corrupt[9] ^= 0x01;
	CHECK(ddc_parse_vcp_reply(corrupt, sizeof corrupt, 0x10, &v) == -1);
	uint8_t empty[12] = {0};
	CHECK(ddc_parse_vcp_reply(empty, sizeof empty, 0x10, &v) == -1);
	CHECK(ddc_parse_vcp_reply(ok, 6, 0x10, &v) == -1);
}

static void test_caps_replies(void) {
	const uint8_t *text = NULL;
	uint8_t reply[DDC_MAX_PACKET] = {0x6E, 0x80 | 8, 0xE3, 0x00, 0x20, '(', 'p', 'r', 'o', 't'};
	reply[10] = reply_checksum(reply, 10);
	CHECK(ddc_parse_caps_reply(reply, sizeof reply, 0x0020, &text) == 5);
	CHECK(text == reply + 5);

	// A fragment for a different offset is a stale reply.
	CHECK(ddc_parse_caps_reply(reply, sizeof reply, 0x0000, &text) == -1);

	// A fragment with no text marks the end of the string.
	uint8_t end[6] = {0x6E, 0x83, 0xE3, 0x00, 0x40, 0};
	end[5] = reply_checksum(end, 5);
	CHECK(ddc_parse_caps_reply(end, sizeof end, 0x0040, &text) == 0);

	uint8_t corrupt[DDC_MAX_PACKET];
	memcpy(corrupt, reply, sizeof reply);
	corrupt[6] = 'x';
	CHECK(ddc_parse_caps_reply(corrupt, sizeof corrupt, 0x0020, &text) == -1);
	uint8_t empty[DDC_MAX_PACKET] = {0};
	CHECK(ddc_parse_caps_reply(empty, sizeof empty, 0, &text) == -1);
}

// ---------------------------------------------------------------------------------------------
// Controls and the guard

static void test_timing_replies(void) {
	// Recorded from a Samsung C34J791 showing 3440 x 1440 at 100 Hz.
	const uint8_t recorded[12] = {0x6E, 0x06, 0x4E, 0x03, 0x3A, 0xF2, 0x27, 0x38, 0xA2};
	struct timing timing = {0, 0, 0};
	CHECK(ddc_parse_timing_reply(recorded, sizeof recorded, &timing) == 0);
	CHECK(timing.status == 0x03);
	CHECK(timing.horizontal == 15090);
	CHECK(timing.vertical == 10040);
	CHECK(timing.status & TIMING_HORIZONTAL_POSITIVE);
	CHECK(timing.status & TIMING_VERTICAL_POSITIVE);
	CHECK(!(timing.status & (TIMING_OUT_OF_RANGE | TIMING_UNSTABLE)));

	uint8_t bad[12];
	memcpy(bad, recorded, sizeof bad);
	bad[8] ^= 0x01; // a wrong checksum
	CHECK(ddc_parse_timing_reply(bad, sizeof bad, &timing) == -1);
	memcpy(bad, recorded, sizeof bad);
	bad[2] = 0x02; // a reply to some other request
	CHECK(ddc_parse_timing_reply(bad, sizeof bad, &timing) == -1);
	memcpy(bad, recorded, sizeof bad);
	bad[1] = 0x88; // the wrong length
	CHECK(ddc_parse_timing_reply(bad, sizeof bad, &timing) == -1);
	const uint8_t empty[12] = {0};
	CHECK(ddc_parse_timing_reply(empty, sizeof empty, &timing) == -1);
	CHECK(ddc_parse_timing_reply(recorded, 8, &timing) == -1);
}

static void test_controls(void) {
	CHECK(control_find("brightness")->code == 0x10);
	CHECK(control_find("contrast")->code == 0x12);
	CHECK(control_find("volume")->code == 0x62);
	CHECK(control_find("mute")->code == 0x8D);
	CHECK(control_find("red")->code == 0x16);
	CHECK(control_find("green")->code == 0x18);
	CHECK(control_find("blue")->code == 0x1A);
	CHECK(control_find("color-preset")->code == 0x14);
	CHECK(control_find("picture-mode")->code == 0xDC);
	CHECK(control_find("input")->code == 0x60);
	CHECK(control_find("power")->code == 0xD6);
	CHECK(control_find("sparkle") == NULL);
	CHECK(control_find("") == NULL);
	CHECK(control_for_code(0x62) == control_find("volume"));
	CHECK(control_for_code(0x99) == NULL);

	size_t count = 0;
	const struct control *all = controls_all(&count);
	CHECK(count == 11);
	CHECK_STR(all[0].name, "brightness");

	uint8_t code = 0;
	CHECK(parse_code("0x10", &code) == 0 && code == 0x10);
	CHECK(parse_code("16", &code) == 0 && code == 16);
	CHECK(parse_code("0xFF", &code) == 0 && code == 255);
	CHECK(parse_code("256", &code) != 0);
	CHECK(parse_code("", &code) != 0);
	CHECK(parse_code("abc", &code) != 0);
	CHECK(parse_code("-1", &code) != 0);
	CHECK(parse_code("0x1G", &code) != 0);

	uint16_t value = 0;
	const struct control *mute = control_find("mute");
	CHECK(control_parse_value(mute, "on", &value) == 0 && value == 1);
	CHECK(control_parse_value(mute, "off", &value) == 0 && value == 2);
	CHECK(control_parse_value(mute, "2", &value) == 0 && value == 2);
	CHECK(control_parse_value(control_find("brightness"), "80", &value) == 0 && value == 80);
	CHECK(control_parse_value(control_find("color-preset"), "6500k", &value) == 0 && value == 5);
	CHECK(control_parse_value(control_find("color-preset"), "srgb", &value) == 0 && value == 1);
	CHECK(control_parse_value(NULL, "0x0F", &value) == 0 && value == 15);
	CHECK(control_parse_value(NULL, "65535", &value) == 0 && value == 65535);
	CHECK(control_parse_value(NULL, "65536", &value) != 0);
	CHECK(control_parse_value(NULL, "-1", &value) != 0);
	CHECK(control_parse_value(NULL, "8O", &value) != 0);
	CHECK(control_parse_value(NULL, "", &value) != 0);
	CHECK(control_parse_value(control_find("brightness"), "on", &value) != 0);

	CHECK_STR(control_value_name(mute, 2), "off");
	CHECK_STR(control_value_name(control_find("power"), 4), "off");
	CHECK(control_value_name(control_find("brightness"), 50) == NULL);
	CHECK(control_value_name(NULL, 1) == NULL);
}

static void test_guard(void) {
	// Switching the monitor off: it stops listening and cannot be switched on again by command.
	CHECK(guard_reason(0xD6, 4) != NULL);
	CHECK(guard_reason(0xD6, 2) != NULL);
	CHECK(guard_reason(0xD6, 1) == NULL);
	// Codes that restore factory settings.
	CHECK(guard_reason(0x04, 1) != NULL);
	CHECK(guard_reason(0x05, 1) != NULL);
	CHECK(guard_reason(0x06, 1) != NULL);
	CHECK(guard_reason(0x08, 1) != NULL);
	CHECK(guard_reason(0x0A, 1) != NULL);
	// Manufacturer-private codes, whose effect is unknown.
	CHECK(guard_reason(0xE0, 0) != NULL);
	CHECK(guard_reason(0xFE, 1) != NULL);
	// Ordinary settings.
	CHECK(guard_reason(0x10, 50) == NULL);
	CHECK(guard_reason(0x60, 15) == NULL);
	CHECK(guard_reason(0xDC, 3) == NULL);
}

// ---------------------------------------------------------------------------------------------
// Command line

static int parse(const char *line, struct command *cmd, char *err, size_t errlen) {
	static char copy[256];
	char *argv[16];
	int argc = 0;
	strncpy(copy, line, sizeof copy - 1);
	copy[sizeof copy - 1] = '\0';
	for (char *word = strtok(copy, " "); word && argc < 16; word = strtok(NULL, " ")) argv[argc++] = word;
	return cli_parse(argc, argv, cmd, err, errlen);
}

static void test_cli(void) {
	struct command c;
	char err[256];

	CHECK(parse("", &c, err, sizeof err) == 0 && c.verb == VERB_HELP);
	CHECK(parse("help", &c, err, sizeof err) == 0 && c.verb == VERB_HELP);
	CHECK(parse("--help", &c, err, sizeof err) == 0 && c.verb == VERB_HELP);
	CHECK(parse("-h", &c, err, sizeof err) == 0 && c.verb == VERB_HELP);
	CHECK(parse("list", &c, err, sizeof err) == 0 && c.verb == VERB_LIST);
	CHECK(parse("caps", &c, err, sizeof err) == 0 && c.verb == VERB_CAPS);
	CHECK(parse("scan", &c, err, sizeof err) == 0 && c.verb == VERB_SCAN);
	CHECK(parse("info", &c, err, sizeof err) == 0 && c.verb == VERB_INFO);
	CHECK(parse("info extra", &c, err, sizeof err) == -1);
	CHECK_STR(err, "Too many arguments for info.");
	CHECK(parse("timing", &c, err, sizeof err) == 0 && c.verb == VERB_TIMING);
	CHECK(parse("timing extra", &c, err, sizeof err) == -1);
	CHECK_STR(err, "Too many arguments for timing.");

	CHECK(parse("get brightness", &c, err, sizeof err) == 0);
	CHECK(c.verb == VERB_GET && c.code == 0x10 && c.control == control_find("brightness") && !c.force);

	// A raw code that has a name still gets its control, so values print with their names.
	CHECK(parse("get 0x62", &c, err, sizeof err) == 0 && c.code == 0x62 && c.control == control_find("volume"));
	CHECK(parse("get 0xB6", &c, err, sizeof err) == 0 && c.code == 0xB6 && c.control == NULL);

	CHECK(parse("set mute on", &c, err, sizeof err) == 0 && c.verb == VERB_SET && c.code == 0x8D && c.value == 1);
	CHECK(parse("set brightness 80", &c, err, sizeof err) == 0 && c.value == 80);
	CHECK(parse("set 0xDC 3", &c, err, sizeof err) == 0 && c.code == 0xDC && c.value == 3);

	// --force may appear anywhere.
	CHECK(parse("set power off --force", &c, err, sizeof err) == 0 && c.force && c.value == 4);
	CHECK(parse("--force set power off", &c, err, sizeof err) == 0 && c.force && c.verb == VERB_SET);

	CHECK(parse("chg volume -5", &c, err, sizeof err) == 0 && c.verb == VERB_CHG && c.code == 0x62 && c.delta == -5);
	CHECK(parse("chg volume +5", &c, err, sizeof err) == 0 && c.delta == 5);
	CHECK(parse("chg volume 5", &c, err, sizeof err) == 0 && c.delta == 5);

	CHECK(parse("get", &c, err, sizeof err) != 0);
	CHECK_HAS(err, "get needs");
	CHECK(parse("get sparkle", &c, err, sizeof err) != 0);
	CHECK_HAS(err, "Unknown control 'sparkle'");
	CHECK(parse("get brightness 5", &c, err, sizeof err) != 0);
	CHECK(parse("set brightness", &c, err, sizeof err) != 0);
	CHECK_HAS(err, "set needs");
	CHECK(parse("set brightness loud", &c, err, sizeof err) != 0);
	CHECK_HAS(err, "'loud' is not a value for brightness");
	CHECK(parse("set mute maybe", &c, err, sizeof err) != 0);
	CHECK_HAS(err, "on, off");
	CHECK(parse("chg volume", &c, err, sizeof err) != 0);
	CHECK(parse("chg volume x", &c, err, sizeof err) != 0);
	CHECK_HAS(err, "'x' is not a whole number");
	CHECK(parse("chg volume 0", &c, err, sizeof err) != 0);
	CHECK(parse("frobnicate", &c, err, sizeof err) != 0);
	CHECK_HAS(err, "Unknown command 'frobnicate'");
	CHECK(parse("list extra", &c, err, sizeof err) != 0);

	CHECK_HAS(cli_usage(), "monitorctl set <control> <value>");
	CHECK_HAS(cli_usage(), "--force");
	CHECK_HAS(cli_usage(), "monitorctl info");
	CHECK_HAS(cli_usage(), "monitorctl timing");
}

// ---------------------------------------------------------------------------------------------
// Running commands against the pretend monitor

static void test_get(void) {
	struct fake f = samsung();
	CHECK(run(&f, "get brightness") == EXIT_OK);
	CHECK_STR(out_text, "95\n");
	CHECK_STR(err_text, "");
	CHECK(run(&f, "get 0x8D") == EXIT_OK);
	CHECK_STR(out_text, "2\n");
	CHECK(f.bad_packets == 0);

	CHECK(run(&f, "get red") == EXIT_FAILED);
	CHECK_STR(out_text, "");
	CHECK_HAS(err_text, "does not support red (code 0x16)");
	CHECK(run(&f, "get 0x99") == EXIT_FAILED);
	CHECK_HAS(err_text, "does not support code 0x99");
}

static void test_get_retries_and_failure(void) {
	struct fake f = samsung();
	f.garbage_replies = 2;
	CHECK(run(&f, "get brightness") == EXIT_OK);
	CHECK_STR(out_text, "95\n");

	f.garbage_replies = 100;
	CHECK(run(&f, "get brightness") == EXIT_FAILED);
	CHECK_HAS(err_text, "did not answer");

	struct fake off = samsung();
	off.unreachable = 1;
	CHECK(run(&off, "get brightness") == EXIT_FAILED);
	CHECK_HAS(err_text, "did not answer");
}

static void test_set(void) {
	struct fake f = samsung();
	CHECK(run(&f, "set brightness 80") == EXIT_OK);
	CHECK(fake_find(&f, 0x10)->current == 80);
	CHECK_STR(out_text, "");
	CHECK_STR(err_text, "");

	CHECK(run(&f, "set mute on") == EXIT_OK);
	CHECK(fake_find(&f, 0x8D)->current == 1);
	CHECK(run(&f, "set color-preset 6500k") == EXIT_OK);
	CHECK(fake_find(&f, 0x14)->current == 5);
	CHECK(f.bad_packets == 0);
}

static void test_set_that_the_monitor_ignores(void) {
	struct fake f = samsung();
	CHECK(run(&f, "set input dp1") == EXIT_NOT_APPLIED);
	CHECK(f.sets == 1);
	CHECK_HAS(err_text, "accepted the command, but input is still 4");
	CHECK(run(&f, "set 0x60 17") == EXIT_NOT_APPLIED);
}

static void test_set_unsupported_and_unreachable(void) {
	struct fake f = samsung();
	CHECK(run(&f, "set red 60") == EXIT_FAILED);
	CHECK_HAS(err_text, "does not support red");
	CHECK(f.sets == 0);

	f.unreachable = 1;
	CHECK(run(&f, "set brightness 80") == EXIT_FAILED);
	CHECK_HAS(err_text, "did not answer");
}

static void test_guarded_writes(void) {
	struct fake f = samsung();
	CHECK(run(&f, "set power off") == EXIT_REFUSED);
	CHECK(f.sets == 0);
	CHECK(fake_find(&f, 0xD6)->current == 1);
	CHECK_HAS(err_text, "Refused");
	CHECK_HAS(err_text, "joystick");
	CHECK_HAS(err_text, "--force");

	CHECK(run(&f, "set 0x04 1") == EXIT_REFUSED);
	CHECK_HAS(err_text, "factory settings");
	CHECK(run(&f, "set 0xFE 2") == EXIT_REFUSED);
	CHECK(run(&f, "chg power 3") == EXIT_REFUSED);
	CHECK(f.sets == 0);

	// Switching the monitor on is always allowed.
	CHECK(run(&f, "set power on") == EXIT_OK);

	// With --force the write goes out. The monitor cannot be read afterwards, and that is not an error.
	CHECK(run(&f, "set power off --force") == EXIT_OK);
	CHECK(fake_find(&f, 0xD6)->current == 4);
	CHECK_HAS(err_text, "could not be read back");
}

static void test_chg(void) {
	struct fake f = samsung();
	CHECK(run(&f, "chg brightness -10") == EXIT_OK);
	CHECK(fake_find(&f, 0x10)->current == 85);
	CHECK_STR(out_text, "85\n");

	// The result stays within 0 and the maximum the monitor reports.
	CHECK(run(&f, "chg brightness +40") == EXIT_OK);
	CHECK(fake_find(&f, 0x10)->current == 100);
	CHECK_STR(out_text, "100\n");
	CHECK(run(&f, "chg brightness -500") == EXIT_OK);
	CHECK(fake_find(&f, 0x10)->current == 0);

	// A monitor that reports a maximum of 0 gets no upper limit.
	CHECK(run(&f, "chg 0xB6 2") == EXIT_OK);
	CHECK(fake_find(&f, 0xB6)->current == 5);

	CHECK(run(&f, "chg red 5") == EXIT_FAILED);
	CHECK(run(&f, "chg input 1") == EXIT_NOT_APPLIED);
}

static void test_list(void) {
	struct fake f = samsung();
	CHECK(run(&f, "list") == EXIT_OK);
	CHECK_HAS(out_text, "brightness     95 of 100\n");
	CHECK_HAS(out_text, "volume         50 of 100\n");
	CHECK_HAS(out_text, "mute           off (2)\n");
	CHECK_HAS(out_text, "color-preset   5000k (4)\n");
	CHECK_HAS(out_text, "power          on (1)\n");
	CHECK_HAS(out_text, "red            not supported\n");
	// A value with no name in a named control is shown as a number.
	CHECK_HAS(out_text, "input          4\n");
}

static void test_scan(void) {
	struct fake f = samsung();
	CHECK(run(&f, "scan") == EXIT_OK);
	CHECK_HAS(out_text, "0x10  current=95     max=100    brightness\n");
	CHECK_HAS(out_text, "0xB6  current=3      max=0      \n");
	CHECK(strstr(out_text, "0x16") == NULL);
	int lines = 0;
	for (const char *p = out_text; *p; p++) lines += *p == '\n';
	CHECK(lines == 8);
}

static void test_caps(void) {
	// The string is longer than one 32-byte fragment, so it is read in pieces and joined.
	struct fake f = samsung();
	CHECK(strlen(f.caps) > 64);
	CHECK(run(&f, "caps") == EXIT_OK);
	char expected[256];
	snprintf(expected, sizeof expected, "%s\n", f.caps);
	CHECK_STR(out_text, expected);

	f.garbage_replies = 2;
	CHECK(run(&f, "caps") == EXIT_OK);
	CHECK_STR(out_text, expected);

	f.unreachable = 1;
	CHECK(run(&f, "caps") == EXIT_FAILED);
}

static void test_info(void) {
	struct fake f = samsung();
	char expected[512];
	snprintf(expected, sizeof expected,
		"model          C34J79x\n"
		"resolution     3440 x 1440 at 100 Hz\n"
		"thunderbolt    firmware 61.2\n"
		"capabilities   %s\n",
		f.caps);
	CHECK(run(&f, "info") == EXIT_OK);
	CHECK_STR(out_text, expected);
	CHECK_STR(err_text, "");
	CHECK(f.sets == 0);
}

static void test_info_resolution_forms(void) {
	// Picture-by-picture halves the width and drops the rate, so the line shows it.
	struct fake f = samsung();
	f.info.width = 1720;
	f.info.refresh = 60.0;
	CHECK(run(&f, "info") == EXIT_OK);
	CHECK_HAS(out_text, "resolution     1720 x 1440 at 60 Hz\n");

	// A rate that is not a whole number keeps its decimals, without trailing zeros.
	f.info.refresh = 59.94;
	CHECK(run(&f, "info") == EXIT_OK);
	CHECK_HAS(out_text, "resolution     1720 x 1440 at 59.94 Hz\n");
	f.info.refresh = 143.981;
	CHECK(run(&f, "info") == EXIT_OK);
	CHECK_HAS(out_text, "resolution     1720 x 1440 at 143.98 Hz\n");
	f.info.refresh = 99.996;
	CHECK(run(&f, "info") == EXIT_OK);
	CHECK_HAS(out_text, "resolution     1720 x 1440 at 100 Hz\n");

	// Some displays report no rate.
	f.info.refresh = 0;
	CHECK(run(&f, "info") == EXIT_OK);
	CHECK_HAS(out_text, "resolution     1720 x 1440\n");
}

static void test_info_with_facts_missing(void) {
	struct fake f = samsung();
	f.info.thunderbolt[0] = '\0';
	CHECK(run(&f, "info") == EXIT_OK);
	CHECK_HAS(out_text, "thunderbolt    not connected over Thunderbolt\n");
	CHECK_HAS(out_text, "model          C34J79x\n");

	f.info.model[0] = '\0';
	f.info.width = f.info.height = 0;
	CHECK(run(&f, "info") == EXIT_OK);
	CHECK_HAS(out_text, "model          unknown\n");
	CHECK_HAS(out_text, "resolution     unknown\n");
}

static void test_info_when_the_monitor_is_silent(void) {
	// The first three lines come from the computer, so they still print.
	struct fake f = samsung();
	f.unreachable = 1;
	CHECK(run(&f, "info") == EXIT_OK);
	CHECK_STR(out_text,
		"model          C34J79x\n"
		"resolution     3440 x 1440 at 100 Hz\n"
		"thunderbolt    firmware 61.2\n"
		"capabilities   the monitor did not answer\n");
}

static void test_info_when_the_computer_cannot_describe_the_display(void) {
	struct fake f = samsung();
	f.info_fails = 1;
	CHECK(run(&f, "info") == EXIT_FAILED);
	CHECK_STR(out_text, "");
	CHECK_HAS(err_text, "could not describe the display");
}

static void test_timing(void) {
	struct fake f = samsung();
	CHECK(run(&f, "timing") == EXIT_OK);
	CHECK_STR(out_text,
		"line rate      150.9 kHz\n"
		"refresh rate   100.4 Hz\n"
		"sync           horizontal positive, vertical positive\n"
		"signal         stable and in range\n");
	CHECK_STR(err_text, "");
	CHECK(f.sets == 0);
	CHECK(f.bad_packets == 0);

	// Picture-by-picture on the same monitor: 1720 x 1440 at 60 Hz.
	f.timing.horizontal = 8890;
	f.timing.vertical = 6000;
	CHECK(run(&f, "timing") == EXIT_OK);
	CHECK_HAS(out_text, "line rate      88.9 kHz\n");
	CHECK_HAS(out_text, "refresh rate   60 Hz\n");

	// A rate keeps up to two decimals.
	f.timing.horizontal = 6747;
	f.timing.vertical = 5994;
	CHECK(run(&f, "timing") == EXIT_OK);
	CHECK_HAS(out_text, "line rate      67.47 kHz\n");
	CHECK_HAS(out_text, "refresh rate   59.94 Hz\n");
}

static void test_timing_flags(void) {
	struct fake f = samsung();
	f.timing.status = 0x00;
	CHECK(run(&f, "timing") == EXIT_OK);
	CHECK_HAS(out_text, "sync           horizontal negative, vertical negative\n");
	f.timing.status = TIMING_HORIZONTAL_POSITIVE;
	CHECK(run(&f, "timing") == EXIT_OK);
	CHECK_HAS(out_text, "sync           horizontal positive, vertical negative\n");
	f.timing.status = TIMING_VERTICAL_POSITIVE;
	CHECK(run(&f, "timing") == EXIT_OK);
	CHECK_HAS(out_text, "sync           horizontal negative, vertical positive\n");

	f.timing.status = TIMING_OUT_OF_RANGE;
	CHECK(run(&f, "timing") == EXIT_OK);
	CHECK_HAS(out_text, "signal         out of range\n");
	f.timing.status = TIMING_UNSTABLE;
	CHECK(run(&f, "timing") == EXIT_OK);
	CHECK_HAS(out_text, "signal         unstable\n");
	f.timing.status = TIMING_OUT_OF_RANGE | TIMING_UNSTABLE;
	CHECK(run(&f, "timing") == EXIT_OK);
	CHECK_HAS(out_text, "signal         unstable and out of range\n");

	// The four reserved bits carry no meaning.
	f.timing.status = 0x3C | TIMING_HORIZONTAL_POSITIVE | TIMING_VERTICAL_POSITIVE;
	CHECK(run(&f, "timing") == EXIT_OK);
	CHECK_HAS(out_text, "sync           horizontal positive, vertical positive\n");
	CHECK_HAS(out_text, "signal         stable and in range\n");
}

static void test_timing_retries_and_failure(void) {
	struct fake f = samsung();
	f.garbage_replies = 2;
	CHECK(run(&f, "timing") == EXIT_OK);
	CHECK_HAS(out_text, "refresh rate   100.4 Hz\n");

	// A monitor that answers other requests and not this one.
	f.no_timing = 1;
	CHECK(run(&f, "timing") == EXIT_FAILED);
	CHECK_STR(out_text, "");
	CHECK_HAS(err_text, "did not answer the timing request");
	CHECK(run(&f, "get brightness") == EXIT_OK);

	f.no_timing = 0;
	f.unreachable = 1;
	CHECK(run(&f, "timing") == EXIT_FAILED);
	CHECK_HAS(err_text, "did not answer the timing request");
}

static void test_help(void) {
	struct fake f = samsung();
	CHECK(run(&f, "help") == EXIT_OK);
	CHECK_HAS(out_text, "monitorctl list");
	CHECK(run(&f, "get") == EXIT_USAGE);
}

int main(void) {
	test_packets();
	test_vcp_replies();
	test_caps_replies();
	test_timing_replies();
	test_controls();
	test_guard();
	test_cli();
	test_get();
	test_get_retries_and_failure();
	test_set();
	test_set_that_the_monitor_ignores();
	test_set_unsupported_and_unreachable();
	test_guarded_writes();
	test_chg();
	test_list();
	test_scan();
	test_caps();
	test_info();
	test_info_resolution_forms();
	test_info_with_facts_missing();
	test_info_when_the_monitor_is_silent();
	test_info_when_the_computer_cannot_describe_the_display();
	test_timing();
	test_timing_flags();
	test_timing_retries_and_failure();
	test_help();
	printf("%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
