#include "profile.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// An EDID packs the manufacturer's three letters into 15 bits, five bits each, with A as 1.
int profile_id(unsigned vendor, unsigned product, char id[8]) {
	id[0] = '\0';
	if (vendor > 0x7FFF || product > 0xFFFF) return -1;
	char letters[4] = "";
	for (int i = 0; i < 3; i++) {
		unsigned letter = (vendor >> (10 - 5 * i)) & 0x1F;
		if (letter < 1 || letter > 26) return -1;
		letters[i] = (char)('A' + letter - 1);
	}
	snprintf(id, 8, "%s%04X", letters, product);
	return 0;
}

// Adds formatted text at `*used`. After an overflow, `*used` is larger than `size` and stays so.
static void append(char *text, size_t size, size_t *used, const char *format, ...) {
	va_list args;
	va_start(args, format);
	int n = vsnprintf(*used < size ? text + *used : NULL, *used < size ? size - *used : 0, format, args);
	va_end(args);
	*used += n > 0 ? (size_t)n : 0;
}

// Adds "<key> <value>" as one line. The model name and the capability string come from the
// monitor, so any character that could start a new line is replaced with a space.
static void append_fact(char *text, size_t size, size_t *used, const char *key, const char *value) {
	if (!value[0]) return;
	append(text, size, used, "%s ", key);
	for (const char *c = value; *c; c++) append(text, size, used, "%c", iscntrl((unsigned char)*c) ? ' ' : *c);
	append(text, size, used, "\n");
}

int profile_format(const struct profile *profile, char *text, size_t size) {
	static const char *RESULTS[] = {[WRITE_UNTESTED] = "untested", [WRITE_WORKS] = "works", [WRITE_IGNORED] = "ignored", [WRITE_MIXED] = "mixed"};
	size_t used = 0;
	append(text, size, &used, "monitorctl-profile 1\n");
	append_fact(text, size, &used, "id", profile->id);
	append_fact(text, size, &used, "model", profile->model);
	if (profile->year && profile->week) append(text, size, &used, "built %u week %u\n", profile->year, profile->week);
	else if (profile->year) append(text, size, &used, "built %u\n", profile->year);
	append_fact(text, size, &used, "thunderbolt", profile->thunderbolt);
	append_fact(text, size, &used, "caps", profile->caps);
	for (int i = 0; i < profile->count; i++) {
		const struct profile_code *c = &profile->codes[i];
		append(text, size, &used, "code 0x%02X max %u write %s\n", c->code, c->maximum, RESULTS[c->write]);
	}
	return used < size ? (int)used : -1;
}

#define MAX_LINES 400

// Reads a whole number of up to nine digits and nothing else. Returns 0 on success.
static int parse_count(const char *text, unsigned *number) {
	size_t digits = strspn(text, "0123456789");
	if (digits == 0 || digits > 9 || text[digits] != '\0') return -1;
	*number = (unsigned)strtoul(text, NULL, 10);
	return 0;
}

int profile_parse(const char *text, const char *id, struct profile *profile, unsigned *reports) {
	static const struct {
		const char *word;
		enum write_result result;
	} RESULTS[] = {{"untested", WRITE_UNTESTED}, {"works", WRITE_WORKS}, {"ignored", WRITE_IGNORED}, {"mixed", WRITE_MIXED}};

	memset(profile, 0, sizeof *profile);
	*reports = 0;
	int lines = 0, has_id = 0, has_reports = 0;
	char seen[256] = {0};

	for (const char *at = text; *at;) {
		// Take one line, without its line ending. The longest real line is the capability string.
		size_t length = strcspn(at, "\n"), kept = length;
		if (kept > 0 && at[kept - 1] == '\r') kept--;
		char line[4200];
		if (kept >= sizeof line || ++lines > MAX_LINES) return -1;
		memcpy(line, at, kept);
		line[kept] = '\0';
		at += length + (at[length] == '\n');

		if (lines == 1) {
			if (strcmp(line, "monitorctl-profile 1") != 0) return -1;
		} else if (strncmp(line, "id ", 3) == 0) {
			if (has_id || strcmp(line + 3, id) != 0) return -1;
			has_id = 1;
			snprintf(profile->id, sizeof profile->id, "%s", id);
		} else if (strncmp(line, "model ", 6) == 0) {
			snprintf(profile->model, sizeof profile->model, "%s", line + 6);
		} else if (strncmp(line, "reports ", 8) == 0) {
			if (has_reports || parse_count(line + 8, reports) != 0) return -1;
			has_reports = 1;
		} else if (strncmp(line, "code ", 5) == 0) {
			unsigned code, maximum;
			char word[16], exact[64];
			if (sscanf(line, "code 0x%2x max %5u write %15s", &code, &maximum, word) != 3 || maximum > 0xFFFF) return -1;
			// Writing the line back out and comparing refuses every other spelling of it.
			snprintf(exact, sizeof exact, "code 0x%02X max %u write %s", code, maximum, word);
			if (strcmp(exact, line) != 0 || seen[code]) return -1;
			size_t which = 0;
			while (which < sizeof RESULTS / sizeof RESULTS[0] && strcmp(RESULTS[which].word, word) != 0) which++;
			if (which == sizeof RESULTS / sizeof RESULTS[0]) return -1;
			seen[code] = 1;
			profile->codes[profile->count++] = (struct profile_code){(uint8_t)code, (uint16_t)maximum, RESULTS[which].result};
		}
		// Any other line belongs to a fact this version does not use, and is skipped.
	}
	return has_id && profile->count > 0 ? 0 : -1;
}

// Makes one folder. A folder that is already there is fine.
static int make_folder(const char *path) {
	if (mkdir(path, 0755) == 0) return 0;
	struct stat st;
	return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

int profile_save(void *ctx, const char *id, const char *text, char *path, size_t size) {
	(void)ctx;
	if (size > 0) path[0] = '\0';

	// The name becomes a file name, so it may hold only letters, digits and hyphens, and it
	// starts with a letter or a digit.
	if (!isalnum((unsigned char)id[0])) return -1;
	for (const char *c = id; *c; c++)
		if (!isalnum((unsigned char)*c) && *c != '-') return -1;

	char folder[900];
	const char *config = getenv("XDG_CONFIG_HOME");
	if (config && config[0] == '/') {
		if (snprintf(folder, sizeof folder, "%s", config) >= (int)sizeof folder) return -1;
	} else {
		const char *home = getenv("HOME");
		if (!home || home[0] != '/') return -1;
		if (snprintf(folder, sizeof folder, "%s/.config", home) >= (int)sizeof folder) return -1;
	}
	if (make_folder(folder) != 0) return -1;
	size_t length = strlen(folder);
	if (snprintf(folder + length, sizeof folder - length, "/monitorctl") >= (int)(sizeof folder - length)) return -1;
	if (make_folder(folder) != 0) return -1;

	char file_path[1024];
	if (snprintf(file_path, sizeof file_path, "%s/%s.txt", folder, id) >= (int)sizeof file_path) return -1;
	if (strlen(file_path) >= size) return -1;

	FILE *file = fopen(file_path, "w");
	if (!file) return -1;
	int failed = fputs(text, file) == EOF;
	if (fclose(file) != 0) failed = 1;
	if (failed) return -1;
	snprintf(path, size, "%s", file_path);
	return 0;
}
