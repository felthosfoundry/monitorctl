#include "run.h"

#include <string.h>

#include "ddc.h"
#include "problem.h"
#include "profile.h"

#define ATTEMPTS 4
#define REPLY_WAIT_MS 60  // a monitor needs a moment between a request and its reply
#define LONG_WAIT_MS 250  // and some monitors need a longer one
#define RESEND_GAP_MS 10  // the gap between the two copies of a request that is sent twice
#define SETTLE_MS 250     // a monitor needs a moment to apply a write before it reports the new value
#define SCAN_GAP_MS 20
#define PROBE_CODE 0x10   // brightness, which nearly every monitor supports

// How a request is made: how long to wait for the reply, and whether to send the request twice.
// Most monitors answer a single request after the usual wait. Some answer with a null message
// until they are given more time or the request a second time.
struct method {
	unsigned wait_ms;
	int twice;
};

// While no request has been answered, each attempt at a request uses the next of these. The
// first two are the usual method, so a monitor that works today is asked exactly as before, and
// one bad reply at the start does not change how it is asked.
#define RUNGS 5
static const struct method LADDER[RUNGS] = {
	{REPLY_WAIT_MS, 0},
	{REPLY_WAIT_MS, 0},
	{REPLY_WAIT_MS, 1},
	{LONG_WAIT_MS, 0},
	{LONG_WAIT_MS, 1},
};

// The first method that gets a valid reply is used for the rest of the command.
static struct method settled;
static int is_settled;

static struct method method_for(int attempt) {
	return is_settled ? settled : LADDER[attempt];
}

// How many attempts a request gets: one for each rung until a method is settled, and the
// usual number after that.
static int attempts_allowed(void) {
	return is_settled ? ATTEMPTS : RUNGS;
}

static void settle(struct method method) {
	if (is_settled) return;
	settled = method;
	is_settled = 1;
}

// Sends a request the way `method` says. Returns 0 when it went out.
static int send(const struct transport *t, struct method method, const uint8_t *packet, size_t n) {
	if (t->write(t->ctx, packet, n) != 0) return -1;
	if (!method.twice) return 0;
	t->pause(t->ctx, RESEND_GAP_MS);
	return t->write(t->ctx, packet, n);
}

// Sends a request, waits, and reads the reply. Returns 0 when a reply was read.
static int exchange(const struct transport *t, struct method method, const uint8_t *packet, size_t n, uint8_t *reply, size_t size) {
	if (send(t, method, packet, n) != 0) return -1;
	t->pause(t->ctx, method.wait_ms);
	return t->read(t->ctx, reply, size);
}

static const char *NO_ANSWER =
	"The monitor did not answer. It may be off, asleep, showing another input, or not support DDC/CI.\n";

// Reads one code. Returns 0 on success, 1 when the monitor does not support it, -1 on no valid reply.
// Some monitors say "not supported" with a null message. A monitor that has already answered
// another request in this command, and now sends two null messages in a row, is taken to mean that.
static int vcp_get(const struct transport *t, uint8_t code, struct vcp_value *value) {
	int was_settled = is_settled, nulls = 0;
	for (int attempt = 0; attempt < attempts_allowed(); attempt++) {
		uint8_t packet[DDC_MAX_PACKET], reply[12];
		size_t n = ddc_get_request(code, packet);
		struct method method = method_for(attempt);
		if (exchange(t, method, packet, n, reply, sizeof reply) == 0) {
			int result = ddc_parse_vcp_reply(reply, sizeof reply, code, value);
			if (result >= 0) {
				settle(method);
				return result;
			}
			nulls = ddc_is_null_reply(reply, sizeof reply) ? nulls + 1 : 0;
			if (was_settled && nulls == 2) return 1;
		} else {
			nulls = 0;
		}
		t->pause(t->ctx, REPLY_WAIT_MS);
	}
	return -1;
}

// Writes "red (code 0x16)" for a named control and "code 0x99" for a raw one.
static void describe(const struct command *cmd, char *text, size_t size, int with_code) {
	if (cmd->control && with_code) snprintf(text, size, "%s (code 0x%02X)", cmd->control->name, cmd->code);
	else if (cmd->control) snprintf(text, size, "%s", cmd->control->name);
	else snprintf(text, size, "code 0x%02X", cmd->code);
}

// Reads the code before a write, so an unsupported code or a silent monitor is reported plainly.
static int read_first(const struct command *cmd, const struct transport *t, struct vcp_value *value, FILE *err) {
	int result = vcp_get(t, cmd->code, value);
	if (result == 0) return EXIT_OK;
	if (result == 1) {
		char what[64];
		describe(cmd, what, sizeof what, 1);
		fprintf(err, "The monitor does not support %s.\n", what);
	} else {
		fputs(NO_ANSWER, err);
	}
	return EXIT_FAILED;
}

static int refuse_if_guarded(const struct command *cmd, uint16_t value, FILE *err) {
	const char *reason = guard_reason(cmd->code, value);
	if (!reason || cmd->force) return EXIT_OK;
	fprintf(err, "Refused: %s\nAdd --force to send it anyway.\n", reason);
	return EXIT_REFUSED;
}

// Sends a write, then reads the value back. Returns 0 when the monitor now reports `value`, and 1
// when it reports something else, which is left in `*now`. Returns -1 when the write could not be
// sent, and -2 when it was sent and the value could not be read back.
static int vcp_set(const struct transport *t, uint8_t code, uint16_t value, uint16_t *now) {
	uint8_t packet[DDC_MAX_PACKET];
	size_t n = ddc_set_request(code, value, packet);
	// A write is always read first, so by now the method that this monitor answers is known.
	if (send(t, method_for(0), packet, n) != 0) return -1;

	// A monitor answers "accepted" to writes it then ignores, so only a read shows what happened.
	// Some settings take a moment to change, so a mismatch is checked a second time.
	struct vcp_value after = {0, 0, 0};
	int result = -1;
	for (int check = 0; check < 2; check++) {
		t->pause(t->ctx, SETTLE_MS);
		result = vcp_get(t, code, &after);
		if (result != 0 || after.current == value) break;
	}
	if (result != 0) return -2;
	*now = after.current;
	return after.current == value ? 0 : 1;
}

// Sends a write and explains anything that went wrong.
static int write_and_confirm(const struct command *cmd, const struct transport *t, uint16_t value, FILE *err) {
	uint16_t now = 0;
	int result = vcp_set(t, cmd->code, value, &now);
	if (result == -1) {
		fputs(NO_ANSWER, err);
		return EXIT_FAILED;
	}
	if (result == -2) {
		fputs("The command was sent, but the value could not be read back.\n", err);
		return EXIT_OK;
	}
	if (result == 1) {
		char what[64];
		describe(cmd, what, sizeof what, 0);
		fprintf(err, "The monitor accepted the command, but %s is still %u.\n", what, now);
		return EXIT_NOT_APPLIED;
	}
	return EXIT_OK;
}

static int run_get(const struct command *cmd, const struct transport *t, FILE *out, FILE *err) {
	struct vcp_value value;
	int status = read_first(cmd, t, &value, err);
	if (status == EXIT_OK) fprintf(out, "%u\n", value.current);
	return status;
}

static int run_set(const struct command *cmd, const struct transport *t, FILE *err) {
	int status = refuse_if_guarded(cmd, cmd->value, err);
	if (status != EXIT_OK) return status;
	struct vcp_value before;
	status = read_first(cmd, t, &before, err);
	if (status != EXIT_OK) return status;
	return write_and_confirm(cmd, t, cmd->value, err);
}

static int run_chg(const struct command *cmd, const struct transport *t, FILE *out, FILE *err) {
	struct vcp_value before;
	int status = read_first(cmd, t, &before, err);
	if (status != EXIT_OK) return status;

	// Stay within 0 and the maximum the monitor reports. A reported maximum of 0 means "no limit given".
	long limit = before.maximum > 0 ? before.maximum : 0xFFFF;
	long target = (long)before.current + cmd->delta;
	if (target < 0) target = 0;
	if (target > limit) target = limit;

	status = refuse_if_guarded(cmd, (uint16_t)target, err);
	if (status != EXIT_OK) return status;
	status = write_and_confirm(cmd, t, (uint16_t)target, err);
	if (status == EXIT_OK) fprintf(out, "%ld\n", target);
	return status;
}

static int run_list(const struct transport *t, FILE *out, FILE *err) {
	size_t count;
	const struct control *controls = controls_all(&count);
	for (size_t i = 0; i < count; i++) {
		struct vcp_value value;
		int result = vcp_get(t, controls[i].code, &value);
		if (result < 0) {
			fputs(NO_ANSWER, err);
			return EXIT_FAILED;
		}
		const char *name = control_value_name(&controls[i], value.current);
		if (result == 1) fprintf(out, "%-14s not supported\n", controls[i].name);
		else if (name) fprintf(out, "%-14s %s (%u)\n", controls[i].name, name, value.current);
		else if (controls[i].names) fprintf(out, "%-14s %u\n", controls[i].name, value.current);
		else fprintf(out, "%-14s %u of %u\n", controls[i].name, value.current, value.maximum);
	}
	return EXIT_OK;
}

static int run_scan(const struct transport *t, FILE *out, FILE *err) {
	int answered = 0;
	for (int code = 0; code <= 0xFF; code++) {
		struct vcp_value value;
		int result = vcp_get(t, (uint8_t)code, &value);
		if (result >= 0) answered = 1;
		if (result == 0) {
			const struct control *control = control_for_code((uint8_t)code);
			fprintf(out, "0x%02X  current=%-6u max=%-6u %s\n", code, value.current, value.maximum, control ? control->name : "");
		}
		// A monitor that answers neither the first few codes nor the brightness will not answer
		// the rest. The brightness is asked because some monitors stay silent for a code they do
		// not support, and the first few codes are such codes on most monitors.
		if (!answered && code >= 3) {
			if (vcp_get(t, PROBE_CODE, &value) < 0) break;
			answered = 1;
		}
		t->pause(t->ctx, SCAN_GAP_MS);
	}
	if (!answered) {
		fputs(NO_ANSWER, err);
		return EXIT_FAILED;
	}
	return EXIT_OK;
}

// Reads the capability string into `text`. It arrives in fragments of up to 32 bytes, each
// requested by its offset. Returns 0 on success, -1 when the monitor never answered, and -2 when
// it stopped answering part-way through.
static int read_caps(const struct transport *t, char *text, size_t size, size_t *length) {
	*length = 0;
	for (;;) {
		int got = -1;
		const uint8_t *fragment = NULL;
		uint8_t reply[DDC_MAX_PACKET];
		for (int attempt = 0; attempt < attempts_allowed() && got < 0; attempt++) {
			uint8_t packet[DDC_MAX_PACKET];
			size_t n = ddc_caps_request((uint16_t)*length, packet);
			struct method method = method_for(attempt);
			if (exchange(t, method, packet, n, reply, sizeof reply) == 0) got = ddc_parse_caps_reply(reply, sizeof reply, (uint16_t)*length, &fragment);
			if (got >= 0) settle(method);
			t->pause(t->ctx, REPLY_WAIT_MS);
		}
		if (got < 0) return *length == 0 ? -1 : -2;
		if (got == 0 || *length + (size_t)got >= size) break;
		memcpy(text + *length, fragment, (size_t)got);
		*length += (size_t)got;
	}
	// Some monitors pad the end of the string with zero bytes.
	while (*length > 0 && text[*length - 1] == '\0') (*length)--;
	return 0;
}

static int run_caps(const struct transport *t, FILE *out, FILE *err) {
	char text[4096];
	size_t length;
	int result = read_caps(t, text, sizeof text, &length);
	if (result != 0) {
		fputs(result == -1 ? NO_ANSWER : "The monitor stopped answering part-way through its capability string.\n", err);
		return EXIT_FAILED;
	}
	fprintf(out, "%.*s\n", (int)length, text);
	return EXIT_OK;
}

// Writes a refresh rate with up to two decimals and no trailing zeros: 100, 59.94.
static void format_rate(double rate, char *text, size_t size) {
	snprintf(text, size, "%.2f", rate);
	char *end = text + strlen(text) - 1;
	while (*end == '0') *end-- = '\0';
	if (*end == '.') *end = '\0';
}

// The first three lines come from the computer, so they print even when the monitor is silent.
static int run_info(const struct transport *t, FILE *out, FILE *err) {
	struct display_info info;
	memset(&info, 0, sizeof info);
	if (t->info(t->ctx, &info) != 0) {
		fputs("The computer could not describe the display.\n", err);
		return EXIT_FAILED;
	}

	fprintf(out, "%-14s %s\n", "model", info.model[0] ? info.model : "unknown");

	if (info.width == 0 || info.height == 0) {
		fprintf(out, "%-14s unknown\n", "resolution");
	} else if (info.refresh <= 0) {
		fprintf(out, "%-14s %u x %u\n", "resolution", info.width, info.height);
	} else {
		char rate[32];
		format_rate(info.refresh, rate, sizeof rate);
		fprintf(out, "%-14s %u x %u at %s Hz\n", "resolution", info.width, info.height, rate);
	}

	// The firmware is known only for a Thunderbolt device that carries the display's name. When
	// devices are plugged in and none has that name, they are listed, so a monitor whose two
	// names differ is not reported as absent.
	if (info.thunderbolt[0]) {
		fprintf(out, "%-14s firmware %s\n", "thunderbolt", info.thunderbolt);
	} else if (info.device_count > 0) {
		fprintf(out, "%-14s no Thunderbolt device is named %s. Attached:", "thunderbolt", info.model[0] ? info.model : "like this display");
		for (int i = 0; i < info.device_count; i++) {
			const struct attached_device *d = &info.devices[i];
			fprintf(out, "%s %s %s", i ? "," : "", d->vendor, d->model);
			if (d->firmware[0]) fprintf(out, " (firmware %s)", d->firmware);
		}
		fputc('\n', out);
	} else {
		fprintf(out, "%-14s not connected over Thunderbolt\n", "thunderbolt");
	}

	char caps[4096];
	size_t length;
	if (read_caps(t, caps, sizeof caps, &length) == 0) fprintf(out, "%-14s %.*s\n", "capabilities", (int)length, caps);
	else fprintf(out, "%-14s the monitor did not answer\n", "capabilities");
	return EXIT_OK;
}

// Asks the monitor what signal it is receiving. One request returns both rates and two flags.
static int run_timing(const struct transport *t, FILE *out, FILE *err) {
	struct timing timing;
	int got = -1;
	for (int attempt = 0; attempt < attempts_allowed() && got != 0; attempt++) {
		uint8_t packet[DDC_MAX_PACKET], reply[12];
		size_t n = ddc_timing_request(packet);
		struct method method = method_for(attempt);
		if (exchange(t, method, packet, n, reply, sizeof reply) == 0) got = ddc_parse_timing_reply(reply, sizeof reply, &timing);
		if (got == 0) settle(method);
		t->pause(t->ctx, REPLY_WAIT_MS);
	}
	if (got != 0) {
		fputs("The monitor did not answer the timing request. It may be off or asleep, or it may not support this request.\n", err);
		return EXIT_FAILED;
	}

	char rate[32];
	format_rate(timing.horizontal / 100.0, rate, sizeof rate);
	fprintf(out, "%-14s %s kHz\n", "line rate", rate);
	format_rate(timing.vertical / 100.0, rate, sizeof rate);
	fprintf(out, "%-14s %s Hz\n", "refresh rate", rate);
	fprintf(out, "%-14s horizontal %s, vertical %s\n", "sync",
		timing.status & TIMING_HORIZONTAL_POSITIVE ? "positive" : "negative",
		timing.status & TIMING_VERTICAL_POSITIVE ? "positive" : "negative");

	int unstable = timing.status & TIMING_UNSTABLE, out_of_range = timing.status & TIMING_OUT_OF_RANGE;
	const char *signal = "stable and in range";
	if (unstable && out_of_range) signal = "unstable and out of range";
	else if (unstable) signal = "unstable";
	else if (out_of_range) signal = "out of range";
	fprintf(out, "%-14s %s\n", "signal", signal);
	return EXIT_OK;
}

// The value setup writes to test a control: one step away from where the control is now.
// Returns 0, or -1 when the control is in a state setup does not test.
static int test_target(const struct control *control, const struct vcp_value *now, uint16_t *target) {
	if (control->probe == PROBE_TOGGLE) {
		if (now->current != 1 && now->current != 2) return -1;
		*target = now->current == 1 ? 2 : 1;
		return 0;
	}
	// Step up when there is room, and down from the top. A maximum of 0 means "no limit given".
	int room_above = now->maximum > 0 ? now->current < now->maximum : now->current == 0;
	*target = (uint16_t)(room_above ? now->current + 1 : now->current - 1);
	return 0;
}

#define DATABASE_PAGE "https://felthosfoundry.com/monitorctl/"

#define TRACE_ATTEMPTS (RUNGS - 1) // one for each different method

// Tries to read the brightness once with each method, and records how far each try got. Nothing
// is written to the monitor's settings. It stops at the first valid answer, and that method is
// then used for the rest of the command. Returns how many tries were made, and sets `*answered`
// when the last of them got a valid answer.
static int trace(const struct transport *t, struct attempt *attempts, int *answered) {
	memset(attempts, 0, TRACE_ATTEMPTS * sizeof *attempts);
	*answered = 0;
	int tries = 0;
	while (tries < TRACE_ATTEMPTS && !*answered) {
		struct method method = LADDER[tries + 1]; // the first two rungs are the same method
		struct attempt *a = &attempts[tries++];
		a->wait_ms = method.wait_ms;
		a->twice = method.twice;
		uint8_t packet[DDC_MAX_PACKET];
		size_t n = ddc_get_request(PROBE_CODE, packet);
		a->sent = send(t, method, packet, n) == 0;
		a->send_status = t->status ? t->status(t->ctx) : 0;
		if (a->sent) {
			t->pause(t->ctx, method.wait_ms);
			a->read = t->read(t->ctx, a->reply, sizeof a->reply) == 0;
			a->read_status = t->status ? t->status(t->ctx) : 0;
			struct vcp_value value;
			a->valid = a->read && ddc_parse_vcp_reply(a->reply, sizeof a->reply, PROBE_CODE, &value) >= 0;
			if (!a->read) memset(a->reply, 0, sizeof a->reply);
			if (a->valid) {
				settle(method);
				*answered = 1;
			}
		}
		t->pause(t->ctx, REPLY_WAIT_MS);
	}
	return tries;
}

// Says where the request was lost, and what that usually means.
static void explain(FILE *err, const struct attempt *attempts, int tries, const struct display_info *info, const char *name) {
	unsigned status = 0;
	enum cause cause = problem_cause(attempts, tries, &status);
	char code[40] = "";
	if (status) snprintf(code, sizeof code, " (macOS error 0x%08X)", status);

	fputs("The monitor did not answer.\n", err);
	switch (cause) {
	case CAUSE_NOT_SENT:
		fprintf(err, "The Mac could not send the request%s.\n", code);
		fputs("It may be off, asleep, or connected through a dock or adapter that does not carry DDC/CI.\n", err);
		break;
	case CAUSE_NOT_READ:
		fprintf(err, "The Mac sent the request and could not read a reply%s.\n", code);
		fputs("It may be off, asleep, or connected through a dock or adapter that does not carry DDC/CI.\n", err);
		break;
	case CAUSE_EMPTY:
		fputs("The Mac read a reply each time, and every reply was empty.\n", err);
		fputs("The monitor may be showing another input, or DDC/CI may be switched off in the monitor's menu.\n", err);
		break;
	case CAUSE_INVALID:
		fputs("The monitor replied, and its replies were not valid DDC/CI.\n", err);
		fputs("A dock or adapter between the Mac and the monitor can cause this.\n", err);
		break;
	case CAUSE_NULL:
		fputs("It replied to every request with a DDC/CI null message, which means it had no answer ready.\n", err);
		fputs("monitorctl also waited longer and sent each request twice, and neither helped.\n", err);
		fputs("DDC/CI may be switched off in the monitor's menu, or the monitor may not offer it on this input.\n", err);
		break;
	}
	if (info->displays > 1) {
		fprintf(err, "The Mac sees %u external displays. monitorctl talks to one of them, and it may not be %s.\n", info->displays, name);
		fprintf(err, "Unplug the other display%s and run setup again.\n", info->displays > 2 ? "s" : "");
	}
}

// One line of setup's summary for a named control: what this monitor did, and what the database
// says when that adds something. A database record only ever changes this wording.
static void control_line(FILE *out, const char *name, const struct profile_code *here, const struct profile_code *there) {
	static const char *PLAIN[] = {[WRITE_UNTESTED] = "writes not tested", [WRITE_WORKS] = "writes work", [WRITE_IGNORED] = "writes are ignored"};
	if (!here) {
		fprintf(out, "%-14s not supported\n", name);
		return;
	}
	const char *more = "";
	if (there && here->write == WRITE_UNTESTED) {
		if (there->write == WRITE_WORKS) more = " here, and work in the database";
		else if (there->write == WRITE_IGNORED) more = " here, and are ignored in the database";
		else if (there->write == WRITE_MIXED) more = " here, and reports in the database disagree";
	} else if (there && here->write == WRITE_WORKS && there->write == WRITE_IGNORED) {
		more = " here, but are ignored in the database";
	} else if (there && here->write == WRITE_IGNORED && there->write == WRITE_WORKS) {
		more = " here, but work in the database";
	}
	fprintf(out, "%-14s %s%s\n", name, PLAIN[here->write], more);
}

// Saves what setup saw as a problem report, and offers to send it to the monitor database. The
// text that goes out is the text that was saved, and it goes only on a yes or with --upload.
static void report_problem(const struct command *cmd, const struct transport *t, const char *id, const struct display_info *info,
	const struct attempt *attempts, int tries, FILE *out, FILE *err) {
	char text[4096], name[32], path[1024];
	snprintf(name, sizeof name, "%s-problem", id);
	if (problem_format(id, info, attempts, tries, text, sizeof text) < 0 || t->save(t->ctx, name, text, path, sizeof path) != 0) {
		fputs("The problem report could not be saved.\n", err);
		return;
	}
	fprintf(out, "\n%-14s %s\n", "problem", path);
	fflush(out);
	if (cmd->offline) return;

	int send = cmd->upload ? 1 : t->ask(t->ctx,
		"\nSend this problem report to felthosfoundry.com, so monitorctl can be made to work with\n"
		"this monitor? It is the file above. It names the Mac's model and the Thunderbolt devices\n"
		"plugged in, and it holds no serial number. [y/N] ");
	if (send < 0) {
		fputs("\nTo send this problem report, run: monitorctl setup --upload\n", out);
	} else if (send > 0) {
		int sent = t->upload(t->ctx, text);
		if (sent == 0) fprintf(out, "%-14s thank you. A person reads every problem report.\n", "sent");
		else if (sent == 1) fprintf(out, "%-14s the database already has this report from you.\n", "sent");
		else if (sent == -2) fputs("The database did not accept the problem report.\n", err);
		else fputs("The problem report could not be sent. The database could not be reached, or it is not taking reports right now.\n", err);
	}
}

// Finds out what the monitor supports and saves the result as a profile.
// It reads every code, then tests writes on the controls marked for it in controls.c. Each tested
// control moves one step and is put back, and so is any watched control that a test changed as a
// side effect. No other code is written.
// Unless --offline is given, it also asks the monitor database what other people found on the
// same monitor. The answer is shown next to this monitor's results and changes nothing else.
// At the end it offers to send the saved profile to the database, and sends it only on a yes.
// When the monitor does not answer at all, it says where the request was lost, and saves and
// offers a problem report in place of a profile.
static int run_setup(const struct command *cmd, const struct transport *t, FILE *out, FILE *err) {
	struct display_info info;
	struct profile profile;
	memset(&info, 0, sizeof info);
	memset(&profile, 0, sizeof profile);
	if (t->info(t->ctx, &info) != 0 || profile_id(info.vendor, info.product, profile.id) != 0) {
		fputs("The computer could not identify the display, so there is no name to save a profile under.\n", err);
		return EXIT_FAILED;
	}
	snprintf(profile.model, sizeof profile.model, "%s", info.model);
	snprintf(profile.thunderbolt, sizeof profile.thunderbolt, "%s", info.thunderbolt);
	profile.year = info.year;
	profile.week = info.week;

	// Ask the database first, so a slow network never sits between a test write and its undo.
	enum { NOT_ASKED, FOUND, NO_REPORTS, UNREACHABLE, UNREADABLE } database = NOT_ASKED;
	struct profile known;
	unsigned known_reports = 0;
	int known_slot[256] = {0};
	if (!cmd->offline) {
		fprintf(out, "Asking felthosfoundry.com what is known about %s.\n", profile.id);
		fflush(out);
		char record[32768];
		int answer = t->lookup(t->ctx, profile.id, record, sizeof record);
		if (answer < 0) database = UNREACHABLE;
		else if (answer > 0) database = NO_REPORTS;
		else if (profile_parse(record, profile.id, &known, &known_reports) != 0) database = UNREADABLE;
		else database = FOUND;
		if (database == FOUND)
			for (int i = 0; i < known.count; i++) known_slot[known.codes[i].code] = i + 1;
	}

	fputs("Reading every code, then testing a few writes. This takes about half a minute.\n"
	      "The picture may flicker. Every setting that is tested is put back.\n\n", out);
	fflush(out);

	// Read every code. `slot` is where a supported code sits in the profile, plus one.
	uint16_t original[256];
	int slot[256] = {0};
	int answered = 0;
	for (int code = 0; code <= 0xFF; code++) {
		struct vcp_value value;
		int result = vcp_get(t, (uint8_t)code, &value);
		if (result >= 0) answered = 1;
		if (result == 0) {
			profile.codes[profile.count] = (struct profile_code){(uint8_t)code, value.maximum, WRITE_UNTESTED};
			original[code] = value.current;
			slot[code] = ++profile.count;
		}
		// When the first few codes get no answer, the brightness decides. Some monitors stay
		// silent for a code they do not support, and the first few are such codes on most
		// monitors. Each method is tried on the brightness, and every try is recorded.
		if (!answered && code >= 3) {
			struct attempt attempts[TRACE_ATTEMPTS];
			int tries = trace(t, attempts, &answered);
			if (!answered) {
				explain(err, attempts, tries, &info, profile.model[0] ? profile.model : profile.id);
				report_problem(cmd, t, profile.id, &info, attempts, tries, out, err);
				return EXIT_FAILED;
			}
		}
		t->pause(t->ctx, SCAN_GAP_MS);
	}

	size_t caps_length = 0;
	if (read_caps(t, profile.caps, sizeof profile.caps - 1, &caps_length) != 0) caps_length = 0;
	profile.caps[caps_length] = '\0';

	// Test one control at a time. Its value is read again first, because an earlier test may
	// have changed it: on some monitors a volume write switches mute off.
	size_t count;
	const struct control *controls = controls_all(&count);
	for (size_t i = 0; i < count; i++) {
		const struct control *control = &controls[i];
		if (!slot[control->code] || (control->probe != PROBE_STEP && control->probe != PROBE_TOGGLE)) continue;
		struct vcp_value now;
		uint16_t target, after = 0;
		if (vcp_get(t, control->code, &now) != 0 || test_target(control, &now, &target) != 0) continue;
		int result = vcp_set(t, control->code, target, &after);
		struct profile_code *entry = &profile.codes[slot[control->code] - 1];
		if (result == 0) entry->write = WRITE_WORKS;
		else if (result == 1 && after == now.current) entry->write = WRITE_IGNORED;
		if (result >= 0 && after != original[control->code]) vcp_set(t, control->code, original[control->code], &after);
	}

	// Put back anything that is still different from where it started.
	int left_changed = 0;
	for (size_t i = 0; i < count; i++) {
		const struct control *control = &controls[i];
		if (!slot[control->code] || control->probe == PROBE_NONE) continue;
		struct vcp_value now;
		if (vcp_get(t, control->code, &now) != 0 || now.current == original[control->code]) continue;
		uint16_t after = now.current;
		if (vcp_set(t, control->code, original[control->code], &after) != 0) {
			fprintf(err, "Could not put %s back to %u. It is now %u.\n", control->name, original[control->code], after);
			left_changed = 1;
		}
	}

	fprintf(out, "%-14s %s (%s)\n", "monitor", profile.model[0] ? profile.model : "unknown model", profile.id);
	if (database == FOUND) fprintf(out, "%-14s %u report%s for this monitor: " DATABASE_PAGE "%s\n", "database", known_reports, known_reports == 1 ? "" : "s", profile.id);
	else if (database == NO_REPORTS) fprintf(out, "%-14s no reports for this monitor yet. Yours would be the first.\n", "database");
	else if (database == UNREACHABLE) fprintf(out, "%-14s could not be reached\n", "database");
	else if (database == UNREADABLE) fprintf(out, "%-14s sent an answer monitorctl could not read\n", "database");
	else fprintf(out, "%-14s not asked, because of --offline\n", "database");
	// Say so when this monitor is not asked in the usual way, because it explains a slower run.
	if (settled.wait_ms != REPLY_WAIT_MS && settled.twice) fprintf(out, "%-14s answered after a %u ms wait, with each request sent twice\n", "requests", settled.wait_ms);
	else if (settled.wait_ms != REPLY_WAIT_MS) fprintf(out, "%-14s answered after a %u ms wait, not the usual %u ms\n", "requests", settled.wait_ms, (unsigned)REPLY_WAIT_MS);
	else if (settled.twice) fprintf(out, "%-14s answered when each request was sent twice\n", "requests");
	fprintf(out, "%-14s %d can be read\n", "codes", profile.count);
	for (size_t i = 0; i < count; i++) {
		int here = slot[controls[i].code], there = known_slot[controls[i].code];
		control_line(out, controls[i].name, here ? &profile.codes[here - 1] : NULL, there ? &known.codes[there - 1] : NULL);
	}

	char text[16384], path[1024];
	if (profile_format(&profile, text, sizeof text) < 0 || t->save(t->ctx, profile.id, text, path, sizeof path) != 0) {
		fputs("The profile could not be saved.\n", err);
		return EXIT_FAILED;
	}
	fprintf(out, "%-14s %s\n", "profile", path);
	fflush(out);

	// Offer to send the profile. The text that goes out is the text that was just saved.
	int send_failed = 0;
	if (!cmd->offline) {
		int send = cmd->upload ? 1 : t->ask(t->ctx,
			"\nSend this profile to the monitor database at felthosfoundry.com, so the next person\n"
			"with this monitor knows what works? It is the file above, and it holds no serial number. [y/N] ");
		if (send < 0) {
			fputs("\nTo send this profile to the monitor database, run: monitorctl setup --upload\n", out);
		} else if (send > 0) {
			int sent = t->upload(t->ctx, text);
			if (sent == 0) fprintf(out, "%-14s thank you. A person reads it before it appears on the site.\n", "sent");
			else if (sent == 1) fprintf(out, "%-14s the database already has this report from you.\n", "sent");
			else if (sent == -2) fputs("The database did not accept the profile.\n", err);
			else fputs("The profile could not be sent. The database could not be reached, or it is not taking reports right now.\n", err);
			send_failed = sent < 0;
		}
	}
	if (left_changed) return EXIT_NOT_APPLIED;
	return send_failed ? EXIT_FAILED : EXIT_OK;
}

int run_command(const struct command *cmd, const struct transport *t, FILE *out, FILE *err) {
	// Each command finds out afresh how the monitor wants to be asked.
	is_settled = 0;
	settled = LADDER[0];
	switch (cmd->verb) {
	case VERB_HELP:
		fputs(cli_usage(), out);
		return EXIT_OK;
	case VERB_LIST:
		return run_list(t, out, err);
	case VERB_GET:
		return run_get(cmd, t, out, err);
	case VERB_SET:
		return run_set(cmd, t, err);
	case VERB_CHG:
		return run_chg(cmd, t, out, err);
	case VERB_CAPS:
		return run_caps(t, out, err);
	case VERB_SCAN:
		return run_scan(t, out, err);
	case VERB_INFO:
		return run_info(t, out, err);
	case VERB_TIMING:
		return run_timing(t, out, err);
	case VERB_SETUP:
		return run_setup(cmd, t, out, err);
	}
	return EXIT_USAGE;
}
