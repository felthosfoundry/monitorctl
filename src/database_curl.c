// Talks to the monitor database over HTTPS by running the curl that ships with macOS, so
// monitorctl needs no networking library of its own.
// A lookup sends a monitor's identifier and nothing else. An upload sends one profile file.
// What comes back from a lookup is only text for profile_parse to judge.
#include "database_curl.h"

#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

// A build for testing can point these at a local copy of the site.
#ifndef DATABASE_URL
#define DATABASE_URL "https://felthosfoundry.com/monitorctl/api"
#endif
#ifndef DATABASE_PROTOCOLS
#define DATABASE_PROTOCOLS "=https"
#endif

// The options every request shares. --disable keeps a personal curl configuration file out of
// it. No redirects are followed, and the status code arrives on a last line of its own.
#define COMMON "curl", "--disable", "--silent", "--proto", DATABASE_PROTOCOLS, "--user-agent", "monitorctl", "--write-out", "\n%{http_code}"

// Runs curl with `arguments`, feeding it `input` when that is not NULL. The reply's body is left
// in `text` and its status code in `*code`. Returns 0 when curl finished and the reply fitted.
static int run_curl(char *const arguments[], const char *input, char *text, size_t size, long *code) {
	int in[2] = {-1, -1}, out[2];
	if (pipe(out) != 0) return -1;
	if (input && pipe(in) != 0) {
		close(out[0]);
		close(out[1]);
		return -1;
	}

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	if (input) {
		posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
		posix_spawn_file_actions_addclose(&actions, in[0]);
		posix_spawn_file_actions_addclose(&actions, in[1]);
	} else {
		posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	}
	posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
	posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addclose(&actions, out[0]);
	posix_spawn_file_actions_addclose(&actions, out[1]);

	pid_t child;
	int started = posix_spawn(&child, "/usr/bin/curl", &actions, NULL, arguments, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(out[1]);
	if (input) close(in[0]);
	if (started != 0) {
		close(out[0]);
		if (input) close(in[1]);
		return -1;
	}

	if (input) {
		// A profile is far smaller than a pipe holds, so this cannot block on curl's reply.
		// If curl has already gone, the write fails quietly and the exit status tells.
		void (*before)(int) = signal(SIGPIPE, SIG_IGN);
		size_t length = strlen(input), written = 0;
		while (written < length) {
			ssize_t put = write(in[1], input + written, length - written);
			if (put <= 0) break;
			written += (size_t)put;
		}
		close(in[1]);
		signal(SIGPIPE, before);
	}

	// Read everything, keeping only what fits. A reply that does not fit is not an answer.
	size_t length = 0;
	int too_long = 0;
	char spare[512];
	for (;;) {
		ssize_t got = length < size - 1 ? read(out[0], text + length, size - 1 - length) : read(out[0], spare, sizeof spare);
		if (got <= 0) break;
		if (length < size - 1) length += (size_t)got;
		else too_long = 1;
	}
	close(out[0]);
	text[length] = '\0';

	int status = 0;
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || too_long) return -1;

	char *last = strrchr(text, '\n');
	if (!last) return -1;
	*code = strtol(last + 1, NULL, 10);
	*last = '\0';
	return 0;
}

int database_lookup(void *ctx, const char *id, char *text, size_t size) {
	(void)ctx;
	if (size < 16) return -1;

	// The identifier goes into the address, so it may hold only letters and digits.
	if (!id[0] || strlen(id) > 16) return -1;
	for (const char *c = id; *c; c++)
		if (!isalnum((unsigned char)*c)) return -1;
	char url[160];
	snprintf(url, sizeof url, DATABASE_URL "/monitors/%s.txt", id);

	char *const arguments[] = {COMMON, "--max-time", "6", "--max-filesize", "32768", url, NULL};
	long code = 0;
	if (run_curl(arguments, NULL, text, size, &code) != 0) return -1;
	if (code == 200) return 0;
	return code == 404 ? 1 : -1;
}

int database_upload(void *ctx, const char *text) {
	(void)ctx;
	char *const arguments[] = {
		COMMON, "--max-time", "10", "--max-filesize", "4096",
		"--header", "Content-Type: text/plain; charset=utf-8",
		// The site refuses a report without this header, which a web page cannot add.
		"--header", "X-Monitorctl-Upload: 1",
		"--data-binary", "@-", DATABASE_URL "/reports", NULL,
	};
	char reply[4096];
	long code = 0;
	if (run_curl(arguments, text, reply, sizeof reply, &code) != 0) return -1;
	if (code == 201) return 0;
	if (code == 200) return 1;
	// 400 and 413 mean the profile itself was refused. Anything else is the site's own trouble.
	return code == 400 || code == 413 ? -2 : -1;
}
