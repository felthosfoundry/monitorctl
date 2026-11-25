#include <stdio.h>

#include "cli.h"
#include "run.h"
#include "transport_mac.h"

int main(int argc, char **argv) {
	struct command cmd;
	char message[256];
	if (cli_parse(argc - 1, argv + 1, &cmd, message, sizeof message) != 0) {
		fprintf(stderr, "%s\n", message);
		return EXIT_USAGE;
	}

	// Help needs no monitor.
	struct transport transport = {0};
	if (cmd.verb != VERB_HELP && transport_mac_open(&transport) != 0) {
		fputs("No external display was found. It may be off, asleep or unplugged.\n", stderr);
		return EXIT_FAILED;
	}
	return run_command(&cmd, &transport, stdout, stderr);
}
