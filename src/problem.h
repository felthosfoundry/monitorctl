// What setup saw when a monitor did not answer: a few traced attempts to read one setting, and
// what they say about where the request was lost.
#ifndef PROBLEM_H
#define PROBLEM_H

#include <stdint.h>

// One attempt to read the brightness. Each step is tried only when the one before it worked.
struct attempt {
	int sent;             // the request went out
	unsigned send_status; // the operating system's code for the write, or 0 for success or unknown
	int read;             // a reply was read
	unsigned read_status; // the operating system's code for the read
	uint8_t reply[12];    // the reply's bytes, all zero when none was read
	int valid;            // the reply was a valid DDC/CI answer
};

// Where the request was lost, judged over all the attempts.
enum cause {
	CAUSE_NOT_SENT, // the computer could not send any request
	CAUSE_NOT_READ, // requests went out, and no reply could be read
	CAUSE_EMPTY,    // replies were read, and every byte of them was zero
	CAUSE_INVALID,  // replies had bytes in them, and none was valid DDC/CI
	CAUSE_UNSTEADY, // an attempt got a valid answer, after earlier requests had all failed
};

// Returns the cause, and writes to `*status` the operating system's code that goes with it, or 0.
enum cause problem_cause(const struct attempt *attempts, int count, unsigned *status);

#endif
