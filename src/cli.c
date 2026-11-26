#include "cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *cli_usage(void) {
	return "monitorctl changes an external monitor's settings from an Apple Silicon Mac.\n"
	       "\n"
	       "  monitorctl list                     show every named control and its value\n"
	       "  monitorctl get <control>            print a control's value\n"
	       "  monitorctl set <control> <value>    set a control to a value\n"
	       "  monitorctl chg <control> <change>   raise or lower a control, for example -10\n"
	       "  monitorctl scan                     read every code and list what the monitor supports\n"
	       "  monitorctl caps                     print the monitor's own capability string\n"
	       "  monitorctl info                     show the model, resolution, Thunderbolt firmware\n"
	       "                                      and capability string\n"
	       "  monitorctl timing                   show the signal the monitor says it is receiving\n"
	       "  monitorctl setup                    test what the monitor supports and save a profile\n"
	       "  monitorctl help                     show this text\n"
	       "\n"
	       "A control is a name from 'monitorctl list', or a raw code such as 0x10.\n"
	       "A value is a number, or a name where the control has them, such as: set mute on\n"
	       "\n"
	       "After a write, monitorctl reads the value back and says when the monitor ignored it.\n"
	       "\n"
	       "  --force   send a write that is refused by default: switching the monitor off,\n"
	       "            restoring factory settings, or a manufacturer-private code.\n"
	       "  --offline with setup, do not ask felthosfoundry.com what is known about the\n"
	       "            monitor. Without it, setup sends the monitor's identifier and no more.\n"
	       "\n"
	       "Exit codes: 0 done, 1 no answer or unsupported, 2 wrong usage,\n"
	       "            3 the monitor ignored the write, 4 refused without --force.\n";
}

static int fail(char *err, size_t errlen, const char *message) {
	snprintf(err, errlen, "%s", message);
	return -1;
}

// Finds the control or raw code that an argument names.
static int parse_target(const char *text, struct command *cmd, char *err, size_t errlen) {
	const struct control *named = control_find(text);
	if (named) {
		cmd->control = named;
		cmd->code = named->code;
		return 0;
	}
	if (parse_code(text, &cmd->code) == 0) {
		cmd->control = control_for_code(cmd->code);
		return 0;
	}
	snprintf(err, errlen, "Unknown control '%s'. Run 'monitorctl list' to see the names, or give a code such as 0x10.", text);
	return -1;
}

static int parse_value(const char *text, struct command *cmd, char *err, size_t errlen) {
	if (control_parse_value(cmd->control, text, &cmd->value) == 0) return 0;
	const char *name = cmd->control ? cmd->control->name : "a raw code";
	if (cmd->control && cmd->control->names) {
		char names[128] = "";
		for (const struct value_name *n = cmd->control->names; n->name; n++) {
			if (names[0]) strncat(names, ", ", sizeof names - strlen(names) - 1);
			strncat(names, n->name, sizeof names - strlen(names) - 1);
		}
		snprintf(err, errlen, "'%s' is not a value for %s. Use %s, or a number.", text, name, names);
	} else {
		snprintf(err, errlen, "'%s' is not a value for %s. Use a number from 0 to 65535.", text, name);
	}
	return -1;
}

static int parse_delta(const char *text, int *delta, char *err, size_t errlen) {
	char *end = NULL;
	long parsed = strtol(text, &end, 10);
	if (text[0] == '\0' || *end != '\0' || parsed < -65535 || parsed > 65535) {
		snprintf(err, errlen, "'%s' is not a whole number such as 5 or -10.", text);
		return -1;
	}
	if (parsed == 0) return fail(err, errlen, "The change must not be 0.");
	*delta = (int)parsed;
	return 0;
}

static int parse_words(int argc, char **argv, struct command *cmd, char *err, size_t errlen) {
	// --force and --offline may appear anywhere. Everything else keeps its order.
	const char *args[8];
	int n = 0;
	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--force") == 0) {
			cmd->force = 1;
		} else if (strcmp(argv[i], "--offline") == 0) {
			cmd->offline = 1;
		} else if (n < 8) {
			args[n++] = argv[i];
		} else {
			return fail(err, errlen, "Too many arguments. Run 'monitorctl help'.");
		}
	}

	if (n == 0 || strcmp(args[0], "help") == 0 || strcmp(args[0], "--help") == 0 || strcmp(args[0], "-h") == 0) {
		cmd->verb = VERB_HELP;
		return 0;
	}

	const char *verb = args[0];
	static const struct {
		const char *name;
		enum verb verb;
	} plain[] = {{"list", VERB_LIST}, {"caps", VERB_CAPS}, {"scan", VERB_SCAN}, {"info", VERB_INFO}, {"timing", VERB_TIMING}, {"setup", VERB_SETUP}};
	for (size_t i = 0; i < sizeof plain / sizeof plain[0]; i++) {
		if (strcmp(verb, plain[i].name) != 0) continue;
		if (n > 1) {
			snprintf(err, errlen, "Too many arguments for %s.", verb);
			return -1;
		}
		cmd->verb = plain[i].verb;
		return 0;
	}

	if (strcmp(verb, "get") == 0) {
		cmd->verb = VERB_GET;
		if (n < 2) return fail(err, errlen, "get needs a control name or a code, for example: monitorctl get brightness");
		if (n > 2) return fail(err, errlen, "Too many arguments for get.");
		return parse_target(args[1], cmd, err, errlen);
	}

	if (strcmp(verb, "set") == 0) {
		cmd->verb = VERB_SET;
		if (n < 3) return fail(err, errlen, "set needs a control and a value, for example: monitorctl set brightness 80");
		if (n > 3) return fail(err, errlen, "Too many arguments for set.");
		if (parse_target(args[1], cmd, err, errlen) != 0) return -1;
		return parse_value(args[2], cmd, err, errlen);
	}

	if (strcmp(verb, "chg") == 0) {
		cmd->verb = VERB_CHG;
		if (n < 3) return fail(err, errlen, "chg needs a control and a change, for example: monitorctl chg volume -5");
		if (n > 3) return fail(err, errlen, "Too many arguments for chg.");
		if (parse_target(args[1], cmd, err, errlen) != 0) return -1;
		return parse_delta(args[2], &cmd->delta, err, errlen);
	}

	snprintf(err, errlen, "Unknown command '%s'. Run 'monitorctl help'.", verb);
	return -1;
}

int cli_parse(int argc, char **argv, struct command *cmd, char *err, size_t errlen) {
	memset(cmd, 0, sizeof *cmd);
	err[0] = '\0';
	if (parse_words(argc, argv, cmd, err, errlen) != 0) return -1;
	if (cmd->offline && cmd->verb != VERB_SETUP) return fail(err, errlen, "--offline only applies to setup.");
	return 0;
}
