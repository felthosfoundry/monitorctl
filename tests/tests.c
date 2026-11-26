// Tests for everything except the code that talks to real hardware.
// Commands run against a pretend monitor, defined below, that answers DDC/CI packets.
#define _DARWIN_C_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/cli.h"
#include "../src/controls.h"
#include "../src/ddc.h"
#include "../src/problem.h"
#include "../src/profile.h"
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
	int write_limit; // when above 0, the setting accepts this many writes and ignores the rest
	int writes;      // how many writes it has accepted
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
	int no_caps;                     // the monitor does not answer capability requests
	int wrote[256];                  // how many set requests arrived for each code
	int volume_unmutes;              // like the Samsung: a volume write switches mute off
	int gain_sets_preset;            // a colour gain write switches the colour preset to user1
	char saved_id[32];               // the name the last profile was saved under
	char saved[16384];               // the text of the last saved profile
	int saves;                       // how many profiles were saved
	int save_fails;                  // the computer cannot save a profile
	const char *db_record;           // what the monitor database holds for this monitor, or NULL
	int db_unreachable;              // the monitor database cannot be reached
	int lookups;                     // how many times the database was asked
	char lookup_id[16];              // the identifier the database was last asked about
	int answer;                      // what the person says when asked: 1 yes, 0 no, -1 nobody there
	int asks;                        // how many questions were asked
	char asked[512];                 // the last question
	int upload_result;               // what the database says to an upload
	int uploads;                     // how many profiles were sent
	char uploaded[16384];            // the last profile sent
	int no_reply;                    // requests go out, and reading the reply fails
	int noise;                       // replies are bytes that are not DDC/CI
	unsigned os_error;               // the operating system's code for a failed write or read
	unsigned last_status;            // the code for the last write or read, 0 for success
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
	f->last_status = f->unreachable ? f->os_error : 0;
	if (f->unreachable) return -1;
	uint8_t sum = 0x6E ^ 0x51;
	for (size_t i = 0; i + 1 < n; i++) sum ^= packet[i];
	if (n < 3 || packet[n - 1] != sum || (size_t)(packet[0] & 0x7F) != n - 2) f->bad_packets++;
	memset(f->pending, 0, sizeof f->pending);
	memcpy(f->pending, packet, n);
	if (packet[1] == 0x03) {
		f->sets++;
		f->wrote[packet[2]]++;
		struct setting *s = fake_find(f, packet[2]);
		if (s && !s->ignores_writes && (s->write_limit == 0 || s->writes < s->write_limit)) {
			s->current = (uint16_t)(packet[3] << 8 | packet[4]);
			s->writes++;
			struct setting *mute = fake_find(f, 0x8D), *preset = fake_find(f, 0x14);
			if (f->volume_unmutes && packet[2] == 0x62 && mute) mute->current = 2;
			if (f->gain_sets_preset && packet[2] == 0x16 && preset) preset->current = 11;
		}
		// Like the real monitor: once switched off, it stops listening.
		if (packet[2] == 0xD6 && s && s->current != 1) f->unreachable = 1;
	}
	return 0;
}

static int fake_read(void *ctx, uint8_t *buffer, size_t n) {
	struct fake *f = ctx;
	f->last_status = f->unreachable || f->no_reply ? f->os_error : 0;
	if (f->unreachable || f->no_reply) return -1;
	memset(buffer, 0, n);
	if (f->noise) {
		memset(buffer, 0xA5, n);
		return 0;
	}
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
	} else if (f->pending[1] == 0xF3 && !f->no_caps) {
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

static int fake_save(void *ctx, const char *id, const char *text, char *path, size_t size) {
	struct fake *f = ctx;
	if (f->save_fails) return -1;
	f->saves++;
	snprintf(f->saved_id, sizeof f->saved_id, "%s", id);
	snprintf(f->saved, sizeof f->saved, "%s", text);
	snprintf(path, size, "/profiles/%s.txt", id);
	return 0;
}

static int fake_lookup(void *ctx, const char *id, char *text, size_t size) {
	struct fake *f = ctx;
	f->lookups++;
	snprintf(f->lookup_id, sizeof f->lookup_id, "%s", id);
	if (f->db_unreachable) return -1;
	if (!f->db_record) return 1;
	snprintf(text, size, "%s", f->db_record);
	return 0;
}

static int fake_ask(void *ctx, const char *question) {
	struct fake *f = ctx;
	f->asks++;
	snprintf(f->asked, sizeof f->asked, "%s", question);
	return f->answer;
}

static int fake_upload(void *ctx, const char *text) {
	struct fake *f = ctx;
	f->uploads++;
	snprintf(f->uploaded, sizeof f->uploaded, "%s", text);
	return f->upload_result;
}

static unsigned fake_status(void *ctx) {
	struct fake *f = ctx;
	return f->last_status;
}

static struct fake samsung(void) {
	struct fake f = {
		.settings = {
			{.code = 0x10, .current = 95, .maximum = 100}, // brightness
			{.code = 0x12, .current = 75, .maximum = 100}, // contrast
			{.code = 0x62, .current = 50, .maximum = 100}, // volume
			{.code = 0x8D, .current = 2, .maximum = 2},    // mute, off
			{.code = 0x14, .current = 4, .maximum = 11},   // colour preset
			{.code = 0x60, .current = 4, .maximum = 3, .ignores_writes = 1}, // input: this monitor ignores writes to it
			{.code = 0xD6, .current = 1, .maximum = 4},    // power
			{.code = 0xB6, .current = 3, .maximum = 0},    // a code that reports a maximum of 0
		},
		.count = 8,
		.caps = "(prot(monitor)type(lcd)model(FALCON)cmds(01 02 03)vcp(10 12 14(01 04 05) 60(01 03) 62 8D))",
		.info = {.model = "C34J79x", .width = 3440, .height = 1440, .refresh = 100.0, .thunderbolt = "61.2",
			.vendor = 19501, .product = 3870, .year = 2022, .week = 44,
			.computer = "Mac16,7", .system = "27.2.1", .displays = 1, .channels = 1},
		.os_error = 0xE00002C0,
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
	struct transport t = {f, fake_write, fake_read, fake_pause, fake_info, fake_save, fake_lookup, fake_ask, fake_upload, fake_status};
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
	CHECK(parse("setup", &c, err, sizeof err) == 0 && c.verb == VERB_SETUP);
	CHECK(parse("setup extra", &c, err, sizeof err) == -1);
	CHECK_STR(err, "Too many arguments for setup.");
	CHECK(parse("setup", &c, err, sizeof err) == 0 && !c.offline);
	CHECK(parse("setup --offline", &c, err, sizeof err) == 0 && c.verb == VERB_SETUP && c.offline);
	CHECK(parse("--offline setup", &c, err, sizeof err) == 0 && c.offline);
	CHECK(parse("list --offline", &c, err, sizeof err) == -1);
	CHECK_STR(err, "--offline only applies to setup.");
	CHECK(parse("setup", &c, err, sizeof err) == 0 && !c.upload);
	CHECK(parse("setup --upload", &c, err, sizeof err) == 0 && c.verb == VERB_SETUP && c.upload && !c.offline);
	CHECK(parse("list --upload", &c, err, sizeof err) == -1);
	CHECK_STR(err, "--upload only applies to setup.");
	CHECK(parse("setup --upload --offline", &c, err, sizeof err) == -1);
	CHECK_STR(err, "--upload and --offline cannot be used together.");

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
	CHECK_HAS(cli_usage(), "monitorctl setup");
	CHECK_HAS(cli_usage(), "--offline");
	CHECK_HAS(cli_usage(), "--upload");
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

// ---------------------------------------------------------------------------------------------
// Profiles

static void test_profile_id(void) {
	char id[8];
	// The three letters are packed five bits each into the manufacturer number.
	CHECK(profile_id(19501, 3870, id) == 0);
	CHECK_STR(id, "SAM0F1E");
	CHECK(profile_id(4268, 0xA0B8, id) == 0);
	CHECK_STR(id, "DELA0B8");
	CHECK(profile_id(19501, 0, id) == 0);
	CHECK_STR(id, "SAM0000");

	// A number that does not decode to three letters is not a manufacturer.
	CHECK(profile_id(0, 3870, id) == -1);
	CHECK(profile_id(27 << 10 | 1 << 5 | 1, 1, id) == -1);
	CHECK(profile_id(0x8000 | 19501, 3870, id) == -1);
	CHECK(profile_id(19501, 0x10000, id) == -1);
}

static void test_profile_format(void) {
	struct profile p = {
		.id = "SAM0F1E",
		.model = "C34J79x",
		.year = 2022,
		.week = 44,
		.thunderbolt = "61.2",
		.caps = "(prot(monitor)vcp(10 60))",
		.codes = {{0x10, 100, WRITE_WORKS}, {0x60, 3, WRITE_UNTESTED}, {0xDC, 3, WRITE_IGNORED}},
		.count = 3,
	};
	char text[1024];
	CHECK(profile_format(&p, text, sizeof text) == (int)strlen(text));
	CHECK_STR(text,
		"monitorctl-profile 1\n"
		"id SAM0F1E\n"
		"model C34J79x\n"
		"built 2022 week 44\n"
		"thunderbolt 61.2\n"
		"caps (prot(monitor)vcp(10 60))\n"
		"code 0x10 max 100 write works\n"
		"code 0x60 max 3 write untested\n"
		"code 0xDC max 3 write ignored\n");

	// Facts that are not known are left out.
	struct profile bare = {.id = "DELA0B8", .year = 2019, .codes = {{0x10, 100, WRITE_UNTESTED}}, .count = 1};
	CHECK(profile_format(&bare, text, sizeof text) > 0);
	CHECK_STR(text, "monitorctl-profile 1\nid DELA0B8\nbuilt 2019\ncode 0x10 max 100 write untested\n");

	// Text from the monitor cannot start a new line in the file.
	struct profile odd = {.id = "SAM0F1E", .model = "Odd\nid FAKE\tname", .caps = "(a)\r\ncode 0x60 max 3 write works"};
	CHECK(profile_format(&odd, text, sizeof text) > 0);
	CHECK_STR(text, "monitorctl-profile 1\nid SAM0F1E\nmodel Odd id FAKE name\ncaps (a)  code 0x60 max 3 write works\n");

	char small[40];
	CHECK(profile_format(&p, small, sizeof small) == -1);
}

static void test_profile_parse(void) {
	struct profile p;
	unsigned reports = 99;
	const char *text =
		"monitorctl-profile 1\n"
		"id SAM0F1E\n"
		"model C34J79x\n"
		"reports 3\n"
		"something from a newer version\n"
		"code 0x10 max 100 write works\n"
		"code 0x60 max 3 write ignored\n"
		"code 0x62 max 100 write mixed\n"
		"code 0xD6 max 4 write untested\n";
	CHECK(profile_parse(text, "SAM0F1E", &p, &reports) == 0);
	CHECK_STR(p.id, "SAM0F1E");
	CHECK_STR(p.model, "C34J79x");
	CHECK(reports == 3);
	CHECK(p.count == 4);
	CHECK(p.codes[0].code == 0x10 && p.codes[0].maximum == 100 && p.codes[0].write == WRITE_WORKS);
	CHECK(p.codes[1].code == 0x60 && p.codes[1].maximum == 3 && p.codes[1].write == WRITE_IGNORED);
	CHECK(p.codes[2].code == 0x62 && p.codes[2].write == WRITE_MIXED);
	CHECK(p.codes[3].code == 0xD6 && p.codes[3].write == WRITE_UNTESTED);

	// A profile that setup wrote reads back, and Windows line endings are accepted.
	struct profile made = {.id = "SAM0F1E", .model = "C34J79x", .year = 2022, .week = 44, .thunderbolt = "61.2",
		.caps = "(prot(monitor))", .codes = {{0x10, 100, WRITE_WORKS}, {0xDC, 3, WRITE_IGNORED}}, .count = 2};
	char written[1024];
	CHECK(profile_format(&made, written, sizeof written) > 0);
	CHECK(profile_parse(written, "SAM0F1E", &p, &reports) == 0);
	CHECK(p.count == 2 && p.codes[1].code == 0xDC && p.codes[1].write == WRITE_IGNORED);
	CHECK(reports == 0);
	CHECK(profile_parse("monitorctl-profile 1\r\nid SAM0F1E\r\ncode 0x10 max 100 write works\r\n", "SAM0F1E", &p, &reports) == 0);
	CHECK(p.count == 1);

	// Anything else is refused whole.
	static const char *bad[] = {
		"",
		"hello\n",
		"monitorctl-profile 2\nid SAM0F1E\ncode 0x10 max 100 write works\n",
		"monitorctl-profile 1\ncode 0x10 max 100 write works\n",                             // no id
		"monitorctl-profile 1\nid DELA0B8\ncode 0x10 max 100 write works\n",                 // another monitor
		"monitorctl-profile 1\nid SAM0F1E\n",                                                // no codes
		"monitorctl-profile 1\nid SAM0F1E\ncode 0x10 max 100 write maybe\n",
		"monitorctl-profile 1\nid SAM0F1E\ncode 0x1g max 100 write works\n",
		"monitorctl-profile 1\nid SAM0F1E\ncode 16 max 100 write works\n",
		"monitorctl-profile 1\nid SAM0F1E\ncode 0x10 max 65536 write works\n",
		"monitorctl-profile 1\nid SAM0F1E\ncode 0x10 max 100 write works extra\n",
		"monitorctl-profile 1\nid SAM0F1E\ncode 0x10 max 100 write works\ncode 0x10 max 100 write works\n",
		"monitorctl-profile 1\nid SAM0F1E\nreports many\ncode 0x10 max 100 write works\n",
	};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		checks++;
		if (profile_parse(bad[i], "SAM0F1E", &p, &reports) != -1) {
			failures++;
			fprintf(stderr, "FAIL %s:%d: accepted bad record %zu\n", __FILE__, __LINE__, i);
		}
	}

	// A line longer than any real one is refused, and so is a record with too many lines.
	static char huge[9000];
	int at = snprintf(huge, sizeof huge, "monitorctl-profile 1\nid SAM0F1E\nmodel ");
	memset(huge + at, 'x', 6000);
	snprintf(huge + at + 6000, sizeof huge - (size_t)at - 6000, "\ncode 0x10 max 100 write works\n");
	CHECK(profile_parse(huge, "SAM0F1E", &p, &reports) == -1);
	static char many[20000];
	at = snprintf(many, sizeof many, "monitorctl-profile 1\nid SAM0F1E\ncode 0x10 max 100 write works\n");
	for (int i = 0; i < 600; i++) at += snprintf(many + at, sizeof many - (size_t)at, "note line %d\n", i);
	CHECK(profile_parse(many, "SAM0F1E", &p, &reports) == -1);
}

static void read_file(const char *path, char *text, size_t size) {
	text[0] = '\0';
	FILE *file = fopen(path, "r");
	if (!file) return;
	size_t n = fread(text, 1, size - 1, file);
	text[n] = '\0';
	fclose(file);
}

static void test_profile_save(void) {
	char root[512], path[1024], expected[1024], text[256];
	const char *tmp = getenv("TMPDIR");
	snprintf(root, sizeof root, "%s/monitorctl-tests-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
	CHECK(mkdtemp(root) != NULL);

	// With XDG_CONFIG_HOME set, the profile goes in a monitorctl folder there.
	setenv("XDG_CONFIG_HOME", root, 1);
	CHECK(profile_save(NULL, "SAM0F1E", "first\n", path, sizeof path) == 0);
	snprintf(expected, sizeof expected, "%s/monitorctl/SAM0F1E.txt", root);
	CHECK_STR(path, expected);
	read_file(path, text, sizeof text);
	CHECK_STR(text, "first\n");

	// Saving again replaces the file.
	CHECK(profile_save(NULL, "SAM0F1E", "second\n", path, sizeof path) == 0);
	read_file(path, text, sizeof text);
	CHECK_STR(text, "second\n");
	unlink(path);

	// A name that could leave the folder is refused.
	CHECK(profile_save(NULL, "../evil", "x\n", path, sizeof path) == -1);
	CHECK(profile_save(NULL, "", "x\n", path, sizeof path) == -1);
	CHECK(profile_save(NULL, "a/b", "x\n", path, sizeof path) == -1);
	CHECK(profile_save(NULL, "a.b", "x\n", path, sizeof path) == -1);
	CHECK(profile_save(NULL, "-problem", "x\n", path, sizeof path) == -1);
	// A problem report is saved next to the profiles, under the identifier and "-problem".
	CHECK(profile_save(NULL, "SAM0F1E-problem", "x\n", path, sizeof path) == 0);
	CHECK(strstr(path, "/monitorctl/SAM0F1E-problem.txt") != NULL);
	unlink(path);

	// Without XDG_CONFIG_HOME, the folder is .config/monitorctl in the home folder.
	unsetenv("XDG_CONFIG_HOME");
	char *old_home = getenv("HOME");
	char saved_home[512];
	snprintf(saved_home, sizeof saved_home, "%s", old_home ? old_home : "");
	setenv("HOME", root, 1);
	CHECK(profile_save(NULL, "DELA0B8", "third\n", path, sizeof path) == 0);
	snprintf(expected, sizeof expected, "%s/.config/monitorctl/DELA0B8.txt", root);
	CHECK_STR(path, expected);
	read_file(path, text, sizeof text);
	CHECK_STR(text, "third\n");
	unlink(path);

	// A folder that cannot be made is a failure, not a crash.
	char blocker[1024];
	snprintf(blocker, sizeof blocker, "%s/blocker", root);
	FILE *file = fopen(blocker, "w");
	CHECK(file != NULL);
	if (file) fclose(file);
	setenv("XDG_CONFIG_HOME", blocker, 1);
	CHECK(profile_save(NULL, "SAM0F1E", "x\n", path, sizeof path) == -1);
	unlink(blocker);

	unsetenv("XDG_CONFIG_HOME");
	if (saved_home[0]) setenv("HOME", saved_home, 1);
	char folder[1024];
	snprintf(folder, sizeof folder, "%s/monitorctl", root);
	rmdir(folder);
	snprintf(folder, sizeof folder, "%s/.config/monitorctl", root);
	rmdir(folder);
	snprintf(folder, sizeof folder, "%s/.config", root);
	rmdir(folder);
	CHECK(rmdir(root) == 0);
}

// ---------------------------------------------------------------------------------------------
// Setup

static int total_writes_outside(const struct fake *f, const uint8_t *allowed, size_t count) {
	int total = 0;
	for (int code = 0; code < 256; code++) {
		int ok = 0;
		for (size_t i = 0; i < count; i++) ok |= allowed[i] == code;
		if (!ok) total += f->wrote[code];
	}
	return total;
}

static void test_setup(void) {
	struct fake f = samsung();
	CHECK(run(&f, "setup") == EXIT_OK);
	CHECK_STR(err_text, "");
	CHECK(f.bad_packets == 0);

	// The profile is saved under the monitor's identifier, with every code it supports.
	CHECK(f.saves == 1);
	CHECK_STR(f.saved_id, "SAM0F1E");
	char expected[1024];
	snprintf(expected, sizeof expected,
		"monitorctl-profile 1\n"
		"id SAM0F1E\n"
		"model C34J79x\n"
		"built 2022 week 44\n"
		"thunderbolt 61.2\n"
		"caps %s\n"
		"code 0x10 max 100 write works\n"
		"code 0x12 max 100 write works\n"
		"code 0x14 max 11 write untested\n"
		"code 0x60 max 3 write untested\n"
		"code 0x62 max 100 write works\n"
		"code 0x8D max 2 write works\n"
		"code 0xB6 max 0 write untested\n"
		"code 0xD6 max 4 write untested\n",
		f.caps);
	CHECK_STR(f.saved, expected);

	// Each tested control moved by one step and was put back, and nothing else was written.
	CHECK(f.wrote[0x10] == 2 && f.wrote[0x12] == 2 && f.wrote[0x62] == 2 && f.wrote[0x8D] == 2);
	CHECK(f.sets == 8);
	CHECK(fake_find(&f, 0x10)->current == 95);
	CHECK(fake_find(&f, 0x12)->current == 75);
	CHECK(fake_find(&f, 0x62)->current == 50);
	CHECK(fake_find(&f, 0x8D)->current == 2);
	CHECK(fake_find(&f, 0x14)->current == 4);
	CHECK(fake_find(&f, 0x60)->current == 4);
	CHECK(fake_find(&f, 0xD6)->current == 1);

	CHECK_HAS(out_text, "The picture may flicker");
	CHECK_HAS(out_text, "monitor        C34J79x (SAM0F1E)\n");
	CHECK_HAS(out_text, "codes          8 can be read\n");
	CHECK_HAS(out_text, "brightness     writes work\n");
	CHECK_HAS(out_text, "mute           writes work\n");
	CHECK_HAS(out_text, "red            not supported\n");
	CHECK_HAS(out_text, "color-preset   writes not tested\n");
	CHECK_HAS(out_text, "input          writes not tested\n");
	CHECK_HAS(out_text, "power          writes not tested\n");
	CHECK_HAS(out_text, "profile        /profiles/SAM0F1E.txt\n");
}

static void test_setup_never_writes_risky_codes(void) {
	// A monitor that supports every kind of code setup must leave alone.
	struct fake f = samsung();
	f.settings[f.count++] = (struct setting){.code = 0x04, .current = 0, .maximum = 1};  // restore factory defaults
	f.settings[f.count++] = (struct setting){.code = 0xDC, .current = 2, .maximum = 3};  // picture mode
	f.settings[f.count++] = (struct setting){.code = 0xE2, .current = 0, .maximum = 2};  // manufacturer-private
	f.settings[f.count++] = (struct setting){.code = 0xFE, .current = 1, .maximum = 2};  // manufacturer-private
	CHECK(run(&f, "setup") == EXIT_OK);
	static const uint8_t tested[] = {0x10, 0x12, 0x62, 0x8D, 0x16, 0x18, 0x1A};
	CHECK(total_writes_outside(&f, tested, sizeof tested) == 0);
	CHECK_HAS(f.saved, "code 0x04 max 1 write untested\n");
	CHECK_HAS(f.saved, "code 0xDC max 3 write untested\n");
	CHECK_HAS(f.saved, "code 0xE2 max 2 write untested\n");
}

static void test_setup_when_the_monitor_ignores_a_write(void) {
	struct fake f = samsung();
	fake_find(&f, 0x10)->ignores_writes = 1;
	CHECK(run(&f, "setup") == EXIT_OK);
	CHECK_STR(err_text, "");
	CHECK_HAS(f.saved, "code 0x10 max 100 write ignored\n");
	CHECK_HAS(f.saved, "code 0x12 max 100 write works\n");
	CHECK_HAS(out_text, "brightness     writes are ignored\n");
	// The value never moved, so there was nothing to put back.
	CHECK(f.wrote[0x10] == 1);
}

static void test_setup_steps_down_from_the_top(void) {
	struct fake f = samsung();
	fake_find(&f, 0x10)->current = 100;
	fake_find(&f, 0x62)->current = 0;
	fake_find(&f, 0x8D)->current = 1;
	CHECK(run(&f, "setup") == EXIT_OK);
	CHECK_HAS(f.saved, "code 0x10 max 100 write works\n");
	CHECK_HAS(f.saved, "code 0x62 max 100 write works\n");
	CHECK_HAS(f.saved, "code 0x8D max 2 write works\n");
	CHECK(fake_find(&f, 0x10)->current == 100);
	CHECK(fake_find(&f, 0x62)->current == 0);
	CHECK(fake_find(&f, 0x8D)->current == 1);

	// A mute value that is neither on nor off is left alone.
	struct fake odd = samsung();
	fake_find(&odd, 0x8D)->current = 0;
	CHECK(run(&odd, "setup") == EXIT_OK);
	CHECK(odd.wrote[0x8D] == 0);
	CHECK_HAS(odd.saved, "code 0x8D max 2 write untested\n");
}

static void test_setup_puts_back_side_effects(void) {
	// On the Samsung, changing the volume switches mute off.
	struct fake f = samsung();
	f.volume_unmutes = 1;
	fake_find(&f, 0x8D)->current = 1;
	CHECK(run(&f, "setup") == EXIT_OK);
	CHECK_STR(err_text, "");
	CHECK(fake_find(&f, 0x8D)->current == 1);
	CHECK(fake_find(&f, 0x62)->current == 50);

	// On some monitors, changing a colour gain switches the colour preset.
	struct fake g = samsung();
	g.gain_sets_preset = 1;
	g.settings[g.count++] = (struct setting){.code = 0x16, .current = 70, .maximum = 100};
	CHECK(run(&g, "setup") == EXIT_OK);
	CHECK_STR(err_text, "");
	CHECK_HAS(g.saved, "code 0x16 max 100 write works\n");
	CHECK(fake_find(&g, 0x16)->current == 70);
	CHECK(fake_find(&g, 0x14)->current == 4);
	// The preset was written once, to put it back. Its own writes were not tested.
	CHECK(g.wrote[0x14] == 1);
	CHECK_HAS(g.saved, "code 0x14 max 11 write untested\n");
}

static void test_setup_when_a_setting_cannot_be_put_back(void) {
	struct fake f = samsung();
	fake_find(&f, 0x10)->write_limit = 1; // accepts the test write, then ignores the rest
	CHECK(run(&f, "setup") == EXIT_NOT_APPLIED);
	CHECK_HAS(err_text, "Could not put brightness back to 95. It is now 96.");
	// The profile is still saved.
	CHECK(f.saves == 1);
	CHECK_HAS(f.saved, "code 0x10 max 100 write works\n");
	CHECK_HAS(out_text, "profile        /profiles/SAM0F1E.txt\n");
}

static void test_setup_failures(void) {
	struct fake off = samsung();
	off.unreachable = 1;
	CHECK(run(&off, "setup") == EXIT_FAILED);
	CHECK_HAS(err_text, "did not answer");
	CHECK(strstr(off.saved, "monitorctl-profile") == NULL);

	// Without the manufacturer and product numbers there is no name to save under.
	struct fake unknown = samsung();
	unknown.info.vendor = 0;
	CHECK(run(&unknown, "setup") == EXIT_FAILED);
	CHECK_HAS(err_text, "could not identify the display");
	CHECK(unknown.sets == 0 && unknown.saves == 0);

	struct fake blind = samsung();
	blind.info_fails = 1;
	CHECK(run(&blind, "setup") == EXIT_FAILED);
	CHECK_HAS(err_text, "could not identify the display");
	CHECK(blind.sets == 0);

	// When the profile cannot be saved, the settings are still put back.
	struct fake full = samsung();
	full.save_fails = 1;
	CHECK(run(&full, "setup") == EXIT_FAILED);
	CHECK_HAS(err_text, "The profile could not be saved");
	CHECK(fake_find(&full, 0x10)->current == 95);
	CHECK(fake_find(&full, 0x8D)->current == 2);
}

// When nothing answers, setup makes a few more attempts that only read, and says which part failed.
static void test_setup_says_which_part_failed(void) {
	struct fake off = samsung();
	off.unreachable = 1;
	CHECK(run(&off, "setup --offline") == EXIT_FAILED);
	CHECK_HAS(err_text, "The monitor did not answer.\n");
	CHECK_HAS(err_text, "The Mac could not send the request (macOS error 0xE00002C0).\n");
	CHECK_HAS(err_text, "It may be off, asleep");
	CHECK(strstr(err_text, "external displays") == NULL);
	CHECK(off.sets == 0);

	struct fake deaf = samsung();
	deaf.no_reply = 1;
	deaf.os_error = 0xE00002D6;
	CHECK(run(&deaf, "setup --offline") == EXIT_FAILED);
	CHECK_HAS(err_text, "The Mac sent the request and could not read a reply (macOS error 0xE00002D6).\n");
	CHECK(deaf.sets == 0);

	struct fake silent = samsung();
	silent.garbage_replies = 1000000;
	CHECK(run(&silent, "setup --offline") == EXIT_FAILED);
	CHECK_HAS(err_text, "The Mac read a reply each time, and every reply was empty.\n");
	CHECK_HAS(err_text, "DDC/CI may be switched off in the monitor's menu");
	CHECK(silent.sets == 0);

	struct fake noisy = samsung();
	noisy.noise = 1;
	CHECK(run(&noisy, "setup --offline") == EXIT_FAILED);
	CHECK_HAS(err_text, "The monitor replied, and its replies were not valid DDC/CI.\n");
	CHECK(noisy.sets == 0);

	// With two external displays, the tool may be talking to the other one.
	struct fake two = samsung();
	two.unreachable = 1;
	two.info.displays = 2;
	CHECK(run(&two, "setup --offline") == EXIT_FAILED);
	CHECK_HAS(err_text, "The Mac sees 2 external displays. monitorctl talks to one of them, and it may not be C34J79x.\n");
	CHECK_HAS(err_text, "Unplug the other display and run setup again.");

	// Sixteen bad replies fail the scan, and the next attempt then gets through.
	struct fake unsteady = samsung();
	unsteady.garbage_replies = 16;
	CHECK(run(&unsteady, "setup --offline") == EXIT_FAILED);
	CHECK_HAS(err_text, "It answered a later attempt, so its answers come and go. Run setup again.\n");
	CHECK(unsteady.sets == 0);
	CHECK_HAS(unsteady.saved, "cause unsteady\n");

	// A transport that cannot report codes still gets the general message.
	struct fake old = samsung();
	old.unreachable = 1;
	old.os_error = 0;
	CHECK(run(&old, "setup --offline") == EXIT_FAILED);
	CHECK_HAS(err_text, "The Mac could not send the request.\n");
}

static void test_problem_format(void) {
	struct display_info info = {.model = "S34C65xT", .year = 2023, .week = 40, .thunderbolt = "",
		.computer = "Mac16,7", .system = "27.2.1", .displays = 1, .channels = 1, .device_count = 2,
		.devices = {{.vendor = "SAMSUNG", .model = "S34C65T", .firmware = "41.1"}, {.vendor = "CalDigit, Inc.", .model = "TS4"}}};
	struct attempt attempts[3] = {
		{.sent = 0, .send_status = 0xE00002C0},
		{.sent = 1, .read = 0, .read_status = 0xE00002D6},
		{.sent = 1, .read = 1, .reply = {0x6E, 0x88, 0x02, 0, 0x10}},
	};
	char text[4096];
	int length = problem_format("SAM74A3", &info, attempts, 3, text, sizeof text);
	CHECK_STR(text,
		"monitorctl-problem 1\n"
		"id SAM74A3\n"
		"model S34C65xT\n"
		"built 2023 week 40\n"
		"computer Mac16,7\n"
		"system 27.2.1\n"
		"displays 1\n"
		"channels 1\n"
		"device SAMSUNG S34C65T firmware 41.1\n"
		"device CalDigit, Inc. TS4\n"
		"step scan\n"
		"cause invalid\n"
		"try 1 write 0xE00002C0\n"
		"try 2 write 0x00000000 read 0xE00002D6\n"
		"try 3 write 0x00000000 read 0x00000000 reply 6E8802001000000000000000\n");
	CHECK(length == (int)strlen(text));
	CHECK(problem_format("SAM74A3", &info, attempts, 3, text, 40) == -1);

	// Facts the computer does not know are left out, and text from outside cannot start a line.
	struct display_info bare = {.model = "Odd\nid EVIL", .thunderbolt = "61.2", .device_count = 1,
		.devices = {{.vendor = "A\tB", .model = "C\rD\xC3\xA9"}}};
	struct attempt none[1] = {{.sent = 0}};
	CHECK(problem_format("SAM0F1E", &bare, none, 1, text, sizeof text) > 0);
	CHECK_STR(text,
		"monitorctl-problem 1\n"
		"id SAM0F1E\n"
		"model Odd id EVIL\n"
		"thunderbolt 61.2\n"
		"displays 0\n"
		"channels 0\n"
		"device A B C D??\n"
		"step scan\n"
		"cause not-sent\n"
		"try 1 write 0x00000000\n");
}

// When the monitor does not answer, setup saves what it saw and offers to send it.
static void test_setup_saves_a_problem_report(void) {
	struct fake off = samsung();
	off.unreachable = 1;
	off.answer = 0;
	CHECK(run(&off, "setup") == EXIT_FAILED);
	CHECK(off.saves == 1);
	CHECK_STR(off.saved_id, "SAM0F1E-problem");
	CHECK_HAS(off.saved, "monitorctl-problem 1\nid SAM0F1E\nmodel C34J79x\nbuilt 2022 week 44\nthunderbolt 61.2\ncomputer Mac16,7\nsystem 27.2.1\ndisplays 1\nchannels 1\nstep scan\ncause not-sent\n");
	CHECK_HAS(off.saved, "try 1 write 0xE00002C0\ntry 2 write 0xE00002C0\ntry 3 write 0xE00002C0\n");
	CHECK_HAS(out_text, "problem        /profiles/SAM0F1E-problem.txt\n");
	CHECK(off.asks == 1);
	CHECK_HAS(off.asked, "Send this problem report to felthosfoundry.com");
	CHECK_HAS(off.asked, "Thunderbolt devices");
	CHECK_HAS(off.asked, "no serial number");
	CHECK(off.uploads == 0);
	CHECK(off.sets == 0);

	// A yes sends the text that was saved, and nothing else.
	struct fake yes = samsung();
	yes.unreachable = 1;
	yes.answer = 1;
	CHECK(run(&yes, "setup") == EXIT_FAILED);
	CHECK(yes.uploads == 1);
	CHECK_STR(yes.uploaded, yes.saved);
	CHECK_HAS(out_text, "sent           thank you. A person reads every problem report.\n");

	// --upload sends without asking, and --offline neither asks nor sends.
	struct fake unasked = samsung();
	unasked.unreachable = 1;
	CHECK(run(&unasked, "setup --upload") == EXIT_FAILED);
	CHECK(unasked.asks == 0 && unasked.uploads == 1);
	CHECK_HAS(unasked.uploaded, "monitorctl-problem 1\n");

	struct fake offline = samsung();
	offline.unreachable = 1;
	offline.answer = 1;
	CHECK(run(&offline, "setup --offline") == EXIT_FAILED);
	CHECK(offline.saves == 1 && offline.asks == 0 && offline.uploads == 0 && offline.lookups == 0);

	// With nobody to ask, setup says how to send it.
	struct fake piped = samsung();
	piped.unreachable = 1;
	piped.answer = -1;
	CHECK(run(&piped, "setup") == EXIT_FAILED);
	CHECK(piped.uploads == 0);
	CHECK_HAS(out_text, "To send this problem report, run: monitorctl setup --upload\n");

	// What the database answers.
	struct fake known = samsung();
	known.unreachable = 1;
	known.answer = 1;
	known.upload_result = 1;
	CHECK(run(&known, "setup") == EXIT_FAILED);
	CHECK_HAS(out_text, "sent           the database already has this report from you.\n");
	struct fake refused = samsung();
	refused.unreachable = 1;
	refused.answer = 1;
	refused.upload_result = -2;
	CHECK(run(&refused, "setup") == EXIT_FAILED);
	CHECK_HAS(err_text, "The database did not accept the problem report.\n");
	struct fake down = samsung();
	down.unreachable = 1;
	down.answer = 1;
	down.upload_result = -1;
	CHECK(run(&down, "setup") == EXIT_FAILED);
	CHECK_HAS(err_text, "The problem report could not be sent.");

	// When the report cannot be saved, nothing is offered.
	struct fake full = samsung();
	full.unreachable = 1;
	full.save_fails = 1;
	full.answer = 1;
	CHECK(run(&full, "setup") == EXIT_FAILED);
	CHECK_HAS(err_text, "The problem report could not be saved.\n");
	CHECK(full.asks == 0 && full.uploads == 0);

	// A monitor that answers gets a profile and no problem report.
	struct fake fine = samsung();
	CHECK(run(&fine, "setup --offline") == EXIT_OK);
	CHECK_STR(fine.saved_id, "SAM0F1E");
	CHECK(fine.saves == 1);
}

// info lists the Thunderbolt devices it found when none carries the display's name.
static void test_info_lists_thunderbolt_devices(void) {
	struct fake f = samsung();
	f.info.thunderbolt[0] = '\0';
	CHECK(run(&f, "info") == EXIT_OK);
	CHECK_HAS(out_text, "thunderbolt    not connected over Thunderbolt\n");

	f.info.device_count = 1;
	f.info.devices[0] = (struct attached_device){.vendor = "SAMSUNG", .model = "S34C65T", .firmware = "41.1"};
	CHECK(run(&f, "info") == EXIT_OK);
	CHECK_HAS(out_text, "thunderbolt    no Thunderbolt device is named C34J79x. Attached: SAMSUNG S34C65T (firmware 41.1)\n");

	f.info.device_count = 2;
	f.info.devices[1] = (struct attached_device){.vendor = "CalDigit, Inc.", .model = "TS4", .firmware = ""};
	CHECK(run(&f, "info") == EXIT_OK);
	CHECK_HAS(out_text, "Attached: SAMSUNG S34C65T (firmware 41.1), CalDigit, Inc. TS4\n");

	// A device with the display's name is still shown as before.
	struct fake named = samsung();
	named.info.device_count = 1;
	named.info.devices[0] = (struct attached_device){.vendor = "SAMSUNG", .model = "C34J79x", .firmware = "61.2"};
	CHECK(run(&named, "info") == EXIT_OK);
	CHECK_HAS(out_text, "thunderbolt    firmware 61.2\n");
}

static void test_setup_with_facts_missing(void) {
	// A monitor that does not answer capability requests, on a cable that is not Thunderbolt.
	struct fake f = samsung();
	f.no_caps = 1;
	f.info.thunderbolt[0] = '\0';
	f.info.model[0] = '\0';
	f.info.year = f.info.week = 0;
	CHECK(run(&f, "setup") == EXIT_OK);
	CHECK(strstr(f.saved, "caps ") == NULL);
	CHECK(strstr(f.saved, "thunderbolt ") == NULL);
	CHECK(strstr(f.saved, "model ") == NULL);
	CHECK(strstr(f.saved, "built ") == NULL);
	CHECK_HAS(f.saved, "monitorctl-profile 1\nid SAM0F1E\ncode 0x10 max 100 write works\n");
	CHECK_HAS(out_text, "monitor        unknown model (SAM0F1E)\n");
}

// What three other people found on the same monitor. It disagrees with this monitor about
// contrast, and it knows about two controls that setup does not test.
static const char *DATABASE =
	"monitorctl-profile 1\n"
	"id SAM0F1E\n"
	"model C34J79x\n"
	"reports 3\n"
	"code 0x10 max 100 write works\n"
	"code 0x12 max 100 write ignored\n"
	"code 0x14 max 11 write works\n"
	"code 0x60 max 3 write ignored\n"
	"code 0x62 max 100 write mixed\n"
	"code 0xD6 max 4 write untested\n";

static void test_setup_shows_what_the_database_knows(void) {
	struct fake f = samsung();
	f.db_record = DATABASE;
	CHECK(run(&f, "setup") == EXIT_OK);
	CHECK_STR(err_text, "");
	CHECK(f.lookups == 1);
	CHECK_STR(f.lookup_id, "SAM0F1E");
	CHECK_HAS(out_text, "Asking felthosfoundry.com what is known about SAM0F1E.\n");
	CHECK_HAS(out_text, "database       3 reports for this monitor: https://felthosfoundry.com/monitorctl/SAM0F1E\n");
	// Where the database agrees, or its reports disagree with each other, the line is as before.
	CHECK_HAS(out_text, "brightness     writes work\n");
	CHECK_HAS(out_text, "volume         writes work\n");
	CHECK_HAS(out_text, "power          writes not tested\n");
	CHECK_HAS(out_text, "contrast       writes work here, but are ignored in the database\n");
	CHECK_HAS(out_text, "color-preset   writes not tested here, and work in the database\n");
	CHECK_HAS(out_text, "input          writes not tested here, and are ignored in the database\n");

	// The database changes what is said, and nothing else: the same writes, the same profile.
	CHECK(f.sets == 8);
	CHECK(f.wrote[0x60] == 0 && f.wrote[0x14] == 0 && f.wrote[0xD6] == 0);
	CHECK_HAS(f.saved, "code 0x12 max 100 write works\n");
	CHECK_HAS(f.saved, "code 0x14 max 11 write untested\n");
	CHECK_HAS(f.saved, "code 0x60 max 3 write untested\n");
	CHECK(strstr(f.saved, "reports ") == NULL);
}

static void test_setup_database_wording(void) {
	// This monitor ignores brightness writes, and the database says they work.
	struct fake f = samsung();
	fake_find(&f, 0x10)->ignores_writes = 1;
	f.db_record = "monitorctl-profile 1\nid SAM0F1E\nreports 1\ncode 0x10 max 100 write works\ncode 0x60 max 3 write mixed\n";
	CHECK(run(&f, "setup") == EXIT_OK);
	CHECK_HAS(out_text, "database       1 report for this monitor: https://felthosfoundry.com/monitorctl/SAM0F1E\n");
	CHECK_HAS(out_text, "brightness     writes are ignored here, but work in the database\n");
	CHECK_HAS(out_text, "input          writes not tested here, and reports in the database disagree\n");
	CHECK_HAS(out_text, "red            not supported\n");
}

static void test_setup_when_the_database_has_nothing_to_say(void) {
	struct fake none = samsung();
	CHECK(run(&none, "setup") == EXIT_OK);
	CHECK(none.lookups == 1);
	CHECK_HAS(out_text, "database       no reports for this monitor yet. Yours would be the first.\n");
	CHECK_HAS(out_text, "input          writes not tested\n");

	// A database that cannot be reached is not an error.
	struct fake down = samsung();
	down.db_unreachable = 1;
	CHECK(run(&down, "setup") == EXIT_OK);
	CHECK_STR(err_text, "");
	CHECK_HAS(out_text, "database       could not be reached\n");
	CHECK(down.saves == 1);

	// Neither is an answer that is not a record, or a record for a different monitor.
	struct fake junk = samsung();
	junk.db_record = "<html>Service unavailable</html>";
	CHECK(run(&junk, "setup") == EXIT_OK);
	CHECK_HAS(out_text, "database       sent an answer monitorctl could not read\n");
	struct fake wrong = samsung();
	wrong.db_record = "monitorctl-profile 1\nid DELA0B8\nreports 2\ncode 0x60 max 18 write works\n";
	CHECK(run(&wrong, "setup") == EXIT_OK);
	CHECK_HAS(out_text, "database       sent an answer monitorctl could not read\n");
	CHECK_HAS(out_text, "input          writes not tested\n");
}

static void test_setup_offline(void) {
	struct fake f = samsung();
	f.db_record = DATABASE;
	CHECK(run(&f, "setup --offline") == EXIT_OK);
	CHECK(f.lookups == 0);
	CHECK(strstr(out_text, "Asking felthosfoundry.com") == NULL);
	CHECK_HAS(out_text, "database       not asked, because of --offline\n");
	CHECK_HAS(out_text, "input          writes not tested\n");
	CHECK(f.saves == 1);
}

static void test_setup_database_cannot_cause_a_write(void) {
	// A record that claims every risky code works changes no behaviour.
	struct fake f = samsung();
	f.settings[f.count++] = (struct setting){.code = 0x04, .current = 0, .maximum = 1};
	f.settings[f.count++] = (struct setting){.code = 0xE2, .current = 0, .maximum = 2};
	f.db_record = "monitorctl-profile 1\nid SAM0F1E\nreports 9\ncode 0x04 max 1 write works\ncode 0x14 max 11 write works\n"
		"code 0x60 max 3 write works\ncode 0xD6 max 4 write works\ncode 0xE2 max 2 write works\n";
	CHECK(run(&f, "setup") == EXIT_OK);
	static const uint8_t tested[] = {0x10, 0x12, 0x62, 0x8D, 0x16, 0x18, 0x1A};
	CHECK(total_writes_outside(&f, tested, sizeof tested) == 0);
	CHECK(f.sets == 8);
	CHECK(fake_find(&f, 0xD6)->current == 1);

	// The database is not asked when the monitor cannot be identified.
	struct fake unknown = samsung();
	unknown.info.vendor = 0;
	CHECK(run(&unknown, "setup") == EXIT_FAILED);
	CHECK(unknown.lookups == 0);
}

static void test_setup_asks_before_sending(void) {
	// The person says no, which is also what an empty answer means.
	struct fake no = samsung();
	CHECK(run(&no, "setup") == EXIT_OK);
	CHECK(no.asks == 1);
	CHECK(no.uploads == 0);
	CHECK_HAS(no.asked, "felthosfoundry.com");
	CHECK_HAS(no.asked, "no serial number");
	CHECK_HAS(no.asked, "[y/N]");
	CHECK(strstr(out_text, "sent ") == NULL);

	// The person says yes. What is sent is the saved profile, byte for byte.
	struct fake yes = samsung();
	yes.answer = 1;
	yes.db_record = DATABASE;
	CHECK(run(&yes, "setup") == EXIT_OK);
	CHECK_STR(err_text, "");
	CHECK(yes.asks == 1 && yes.uploads == 1);
	CHECK_STR(yes.uploaded, yes.saved);
	// Nothing the database said is in it.
	CHECK(strstr(yes.uploaded, "reports ") == NULL);
	CHECK_HAS(yes.uploaded, "code 0x60 max 3 write untested\n");
	CHECK_HAS(out_text, "sent           thank you. A person reads it before it appears on the site.\n");

	// The question comes after the profile is saved, so the person can read the file first.
	CHECK(strstr(out_text, "profile        ") < strstr(out_text, "sent           "));
}

static void test_setup_without_a_person_to_ask(void) {
	// Not in a terminal: nothing is sent, and setup says how to send it.
	struct fake f = samsung();
	f.answer = -1;
	CHECK(run(&f, "setup") == EXIT_OK);
	CHECK(f.uploads == 0);
	CHECK_HAS(out_text, "To send this profile to the monitor database, run: monitorctl setup --upload\n");

	// --upload sends without asking.
	struct fake script = samsung();
	script.answer = -1;
	CHECK(run(&script, "setup --upload") == EXIT_OK);
	CHECK(script.asks == 0 && script.uploads == 1);
	CHECK_STR(script.uploaded, script.saved);
	CHECK_HAS(out_text, "sent           thank you.");

	// --offline never asks and never sends.
	struct fake offline = samsung();
	offline.answer = 1;
	CHECK(run(&offline, "setup --offline") == EXIT_OK);
	CHECK(offline.asks == 0 && offline.uploads == 0);
	CHECK(strstr(out_text, "monitorctl setup --upload") == NULL);
}

static void test_setup_upload_outcomes(void) {
	struct fake known = samsung();
	known.answer = 1;
	known.upload_result = 1;
	CHECK(run(&known, "setup") == EXIT_OK);
	CHECK_STR(err_text, "");
	CHECK_HAS(out_text, "sent           the database already has this report from you.\n");

	// A send that fails is reported. The profile is saved all the same.
	struct fake refused = samsung();
	refused.answer = 1;
	refused.upload_result = -2;
	CHECK(run(&refused, "setup") == EXIT_FAILED);
	CHECK_HAS(err_text, "The database did not accept the profile.");
	CHECK(refused.saves == 1);
	CHECK(strstr(out_text, "sent ") == NULL);

	struct fake down = samsung();
	down.upload_result = -1;
	CHECK(run(&down, "setup --upload") == EXIT_FAILED);
	CHECK_HAS(err_text, "The profile could not be sent.");
	CHECK(down.saves == 1);
	CHECK(fake_find(&down, 0x10)->current == 95);

	// A setting that could not be put back still decides the exit code.
	struct fake stuck = samsung();
	stuck.answer = 1;
	fake_find(&stuck, 0x10)->write_limit = 1;
	CHECK(run(&stuck, "setup") == EXIT_NOT_APPLIED);
	CHECK(stuck.uploads == 1);

	// Nothing is offered when there is no profile to send.
	struct fake full = samsung();
	full.answer = 1;
	full.save_fails = 1;
	CHECK(run(&full, "setup") == EXIT_FAILED);
	CHECK(full.asks == 0 && full.uploads == 0);
	struct fake off = samsung();
	off.unreachable = 1;
	CHECK(run(&off, "setup --upload") == EXIT_FAILED);
	CHECK(strstr(off.uploaded, "monitorctl-profile") == NULL);
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
	test_profile_id();
	test_profile_format();
	test_profile_save();
	test_profile_parse();
	test_setup();
	test_setup_never_writes_risky_codes();
	test_setup_when_the_monitor_ignores_a_write();
	test_setup_steps_down_from_the_top();
	test_setup_puts_back_side_effects();
	test_setup_when_a_setting_cannot_be_put_back();
	test_setup_failures();
	test_setup_says_which_part_failed();
	test_problem_format();
	test_setup_saves_a_problem_report();
	test_info_lists_thunderbolt_devices();
	test_setup_with_facts_missing();
	test_setup_shows_what_the_database_knows();
	test_setup_database_wording();
	test_setup_when_the_database_has_nothing_to_say();
	test_setup_offline();
	test_setup_database_cannot_cause_a_write();
	test_setup_asks_before_sending();
	test_setup_without_a_person_to_ask();
	test_setup_upload_outcomes();
	test_help();
	printf("%d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
