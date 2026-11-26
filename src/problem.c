#include "problem.h"

#include <stddef.h>

static int is_empty(const struct attempt *attempt) {
	for (size_t i = 0; i < sizeof attempt->reply; i++)
		if (attempt->reply[i] != 0) return 0;
	return 1;
}

// The furthest any attempt got decides the cause: one valid answer outweighs three silences.
enum cause problem_cause(const struct attempt *attempts, int count, unsigned *status) {
	enum cause cause = CAUSE_NOT_SENT;
	*status = 0;
	for (int i = 0; i < count; i++) {
		const struct attempt *a = &attempts[i];
		enum cause this = CAUSE_NOT_SENT;
		if (a->valid) this = CAUSE_UNSTEADY;
		else if (a->read) this = is_empty(a) ? CAUSE_EMPTY : CAUSE_INVALID;
		else if (a->sent) this = CAUSE_NOT_READ;

		if (this > cause || i == 0) {
			cause = this;
			*status = this == CAUSE_NOT_SENT ? a->send_status : this == CAUSE_NOT_READ ? a->read_status : 0;
		}
	}
	return cause;
}
