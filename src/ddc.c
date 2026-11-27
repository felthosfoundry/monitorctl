#include "ddc.h"

#include <string.h>

// DDC/CI checksums are the XOR of every byte in the message, including the address bytes that
// the I2C layer sends and this code never sees. Requests are checked as if sent from 0x51 to 0x6E.
// Replies are checked as if sent to 0x50.
#define DISPLAY_ADDRESS 0x6E
#define HOST_ADDRESS 0x51
#define REPLY_SEED 0x50

#define OP_GET 0x01
#define OP_GET_REPLY 0x02
#define OP_SET 0x03
#define OP_TIMING 0x07
#define OP_TIMING_REPLY 0x4E
#define OP_CAPS 0xF3
#define OP_CAPS_REPLY 0xE3

// Wraps a payload (an opcode and its arguments) with its length byte and checksum.
static size_t build(const uint8_t *payload, uint8_t n, uint8_t *out) {
	out[0] = 0x80 | n;
	memcpy(out + 1, payload, n);
	uint8_t sum = DISPLAY_ADDRESS ^ HOST_ADDRESS;
	for (size_t i = 0; i <= n; i++) sum ^= out[i];
	out[n + 1] = sum;
	return (size_t)n + 2;
}

static uint8_t reply_sum(const uint8_t *reply, size_t n) {
	uint8_t sum = REPLY_SEED;
	for (size_t i = 0; i < n; i++) sum ^= reply[i];
	return sum;
}

size_t ddc_get_request(uint8_t code, uint8_t *out) {
	const uint8_t payload[] = {OP_GET, code};
	return build(payload, sizeof payload, out);
}

size_t ddc_set_request(uint8_t code, uint16_t value, uint8_t *out) {
	const uint8_t payload[] = {OP_SET, code, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
	return build(payload, sizeof payload, out);
}

size_t ddc_caps_request(uint16_t offset, uint8_t *out) {
	const uint8_t payload[] = {OP_CAPS, (uint8_t)(offset >> 8), (uint8_t)(offset & 0xFF)};
	return build(payload, sizeof payload, out);
}

// A get reply is 11 bytes:
//   6E 88 02 <result> <code> <type> <max hi> <max lo> <current hi> <current lo> <checksum>
int ddc_parse_vcp_reply(const uint8_t *reply, size_t len, uint8_t code, struct vcp_value *out) {
	if (len < 11) return -1;
	if (reply[0] != DISPLAY_ADDRESS || reply[1] != 0x88 || reply[2] != OP_GET_REPLY) return -1;
	if (reply[4] != code || reply[10] != reply_sum(reply, 10)) return -1;
	if (reply[3] == 0x01) return 1;
	if (reply[3] != 0x00) return -1;
	out->type = reply[5];
	out->maximum = (uint16_t)(reply[6] << 8 | reply[7]);
	out->current = (uint16_t)(reply[8] << 8 | reply[9]);
	return 0;
}

// A capability reply is:
//   6E <0x80 | n> E3 <offset hi> <offset lo> <n - 3 text bytes> <checksum>
int ddc_parse_caps_reply(const uint8_t *reply, size_t len, uint16_t offset, const uint8_t **text) {
	if (len < 6 || reply[0] != DISPLAY_ADDRESS || reply[2] != OP_CAPS_REPLY) return -1;
	size_t n = reply[1] & 0x7F;
	if ((reply[1] & 0x80) == 0 || n < 3 || n > 35 || len < n + 3) return -1;
	if ((uint16_t)(reply[3] << 8 | reply[4]) != offset) return -1;
	if (reply[n + 2] != reply_sum(reply, n + 2)) return -1;
	*text = reply + 5;
	return (int)(n - 3);
}

size_t ddc_timing_request(uint8_t *out) {
	const uint8_t payload[] = {OP_TIMING};
	return build(payload, sizeof payload, out);
}

// A timing reply is 9 bytes:
//   6E 06 4E <status> <horizontal hi> <horizontal lo> <vertical hi> <vertical lo> <checksum>
// Unlike the other replies, its length byte does not have the top bit set.
int ddc_parse_timing_reply(const uint8_t *reply, size_t len, struct timing *out) {
	if (len < 9) return -1;
	if (reply[0] != DISPLAY_ADDRESS || reply[1] != 0x06 || reply[2] != OP_TIMING_REPLY) return -1;
	if (reply[8] != reply_sum(reply, 8)) return -1;
	out->status = reply[3];
	out->horizontal = (uint16_t)(reply[4] << 8 | reply[5]);
	out->vertical = (uint16_t)(reply[6] << 8 | reply[7]);
	return 0;
}

// A null message is the display's address, a length byte that says "no data", and the checksum:
//   6E 80 BE
int ddc_is_null_reply(const uint8_t *reply, size_t len) {
	return len >= 3 && reply[0] == DISPLAY_ADDRESS && reply[1] == 0x80 && reply[2] == reply_sum(reply, 2);
}
