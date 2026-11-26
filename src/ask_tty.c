#include "ask_tty.h"

#include <stdio.h>
#include <unistd.h>

int ask_tty(void *ctx, const char *question) {
	(void)ctx;
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return -1;
	fputs(question, stdout);
	fflush(stdout);
	char answer[16];
	if (!fgets(answer, sizeof answer, stdin)) return 0;
	return answer[0] == 'y' || answer[0] == 'Y';
}
