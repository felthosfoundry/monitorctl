#include "problem.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "ddc.h"
#include "run.h"

static int is_empty(const struct attempt *attempt) {
	for (size_t i = 0; i < sizeof attempt->reply; i++)
		if (attempt->reply[i] != 0) return 0;
	return 1;
}

// The furthest any attempt got decides the cause: one null message outweighs three silences.
enum cause problem_cause(const struct attempt *attempts, int count, unsigned *status) {
	enum cause cause = CAUSE_NOT_SENT;
	*status = 0;
	for (int i = 0; i < count; i++) {
		const struct attempt *a = &attempts[i];
		enum cause this = CAUSE_NOT_SENT;
		if (a->read && ddc_is_null_reply(a->reply, sizeof a->reply)) this = CAUSE_NULL;
		else if (a->read) this = is_empty(a) ? CAUSE_EMPTY : CAUSE_INVALID;
		else if (a->sent) this = CAUSE_NOT_READ;

		if (this > cause || i == 0) {
			cause = this;
			*status = this == CAUSE_NOT_SENT ? a->send_status : this == CAUSE_NOT_READ ? a->read_status : 0;
		}
	}
	return cause;
}

// Adds formatted text at `*used`. After an overflow, `*used` is larger than `size` and stays so.
static void append(char *text, size_t size, size_t *used, const char *format, ...) {
	va_list args;
	va_start(args, format);
	int n = vsnprintf(*used < size ? text + *used : NULL, *used < size ? size - *used : 0, format, args);
	va_end(args);
	*used += n > 0 ? (size_t)n : 0;
}

// Adds text that came from the monitor or a device. Any character that could start a new line
// is replaced with a space. The site accepts only plain ASCII, so any other byte becomes "?".
static void append_clean(char *text, size_t size, size_t *used, const char *value) {
	for (const char *c = value; *c; c++) {
		unsigned char byte = (unsigned char)*c;
		append(text, size, used, "%c", iscntrl(byte) ? ' ' : byte > 0x7E ? '?' : *c);
	}
}

static void append_fact(char *text, size_t size, size_t *used, const char *key, const char *value) {
	if (!value[0]) return;
	append(text, size, used, "%s ", key);
	append_clean(text, size, used, value);
	append(text, size, used, "\n");
}

int problem_format(const char *id, const struct display_info *info, const struct attempt *attempts, int count, char *text, size_t size) {
	static const char *CAUSES[] = {[CAUSE_NOT_SENT] = "not-sent", [CAUSE_NOT_READ] = "not-read", [CAUSE_EMPTY] = "empty",
		[CAUSE_INVALID] = "invalid", [CAUSE_NULL] = "null"};
	size_t used = 0;
	append(text, size, &used, "monitorctl-problem 1\n");
	append_fact(text, size, &used, "id", id);
	append_fact(text, size, &used, "model", info->model);
	if (info->year && info->week) append(text, size, &used, "built %u week %u\n", info->year, info->week);
	else if (info->year) append(text, size, &used, "built %u\n", info->year);
	append_fact(text, size, &used, "thunderbolt", info->thunderbolt);
	append_fact(text, size, &used, "computer", info->computer);
	append_fact(text, size, &used, "system", info->system);
	append(text, size, &used, "displays %u\n", info->displays);
	append(text, size, &used, "channels %u\n", info->channels);
	for (int i = 0; i < info->device_count; i++) {
		const struct attached_device *device = &info->devices[i];
		append(text, size, &used, "device ");
		append_clean(text, size, &used, device->vendor);
		append(text, size, &used, " ");
		append_clean(text, size, &used, device->model);
		if (device->firmware[0]) {
			append(text, size, &used, " firmware ");
			append_clean(text, size, &used, device->firmware);
		}
		append(text, size, &used, "\n");
	}

	// The scan is the only step that reports a problem today. The line is there so that a later
	// version can name another step without changing the format.
	append(text, size, &used, "step scan\n");
	unsigned status;
	append(text, size, &used, "cause %s\n", CAUSES[problem_cause(attempts, count, &status)]);
	for (int i = 0; i < count; i++) {
		const struct attempt *a = &attempts[i];
		append(text, size, &used, "try %d wait %u%s write 0x%08X", i + 1, a->wait_ms, a->twice ? " twice" : "", a->send_status);
		if (a->sent) append(text, size, &used, " read 0x%08X", a->read_status);
		if (a->sent && a->read) {
			append(text, size, &used, " reply ");
			for (size_t b = 0; b < sizeof a->reply; b++) append(text, size, &used, "%02X", a->reply[b]);
		}
		append(text, size, &used, "\n");
	}
	return used < size ? (int)used : -1;
}
