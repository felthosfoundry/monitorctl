// Fetches a monitor's record over HTTPS by running the curl that ships with macOS, so monitorctl
// needs no networking library of its own. The request carries the monitor's identifier and
// nothing else. What comes back is only text for profile_parse to judge.
#include "lookup_curl.h"

#include <ctype.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define RECORD_URL "https://felthosfoundry.com/monitorctl/api/monitors/%s.txt"

int lookup_curl(void *ctx, const char *id, char *text, size_t size) {
	(void)ctx;
	if (size < 16) return -1;

	// The identifier goes into the address, so it may hold only letters and digits.
	if (!id[0] || strlen(id) > 16) return -1;
	for (const char *c = id; *c; c++)
		if (!isalnum((unsigned char)*c)) return -1;
	char url[128];
	snprintf(url, sizeof url, RECORD_URL, id);

	int ends[2];
	if (pipe(ends) != 0) return -1;
	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, ends[1], STDOUT_FILENO);
	posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addclose(&actions, ends[0]);
	posix_spawn_file_actions_addclose(&actions, ends[1]);

	// --disable keeps a personal curl configuration file out of it. HTTPS only, no redirects, a
	// short time limit, and the status code on a last line of its own.
	char *const arguments[] = {
		"curl", "--disable", "--silent", "--proto", "=https", "--max-time", "6", "--max-filesize", "32768",
		"--user-agent", "monitorctl", "--write-out", "\n%{http_code}", url, NULL,
	};
	pid_t child;
	int started = posix_spawn(&child, "/usr/bin/curl", &actions, NULL, arguments, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(ends[1]);
	if (started != 0) {
		close(ends[0]);
		return -1;
	}

	// Read everything, keeping only what fits. An answer that does not fit is not a record.
	size_t length = 0;
	int too_long = 0;
	char spare[512];
	for (;;) {
		ssize_t got = length < size - 1 ? read(ends[0], text + length, size - 1 - length) : read(ends[0], spare, sizeof spare);
		if (got <= 0) break;
		if (length < size - 1) length += (size_t)got;
		else too_long = 1;
	}
	close(ends[0]);
	text[length] = '\0';

	int status = 0;
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || too_long) return -1;

	char *last = strrchr(text, '\n');
	if (!last) return -1;
	long code = strtol(last + 1, NULL, 10);
	*last = '\0';
	if (code == 200) return 0;
	return code == 404 ? 1 : -1;
}
