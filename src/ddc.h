// DDC/CI packets: how a computer asks a monitor to read or change a setting.
// Everything here is plain byte handling with no hardware access, so it is tested without a monitor.
#ifndef DDC_H
#define DDC_H

#include <stddef.h>
#include <stdint.h>

// The longest packet in either direction: a capability fragment of 32 text bytes plus its framing.
#define DDC_MAX_PACKET 40

// A monitor setting is identified by a one-byte code, called a VCP code. 0x10 is brightness.
struct vcp_value {
	uint16_t current;
	uint16_t maximum;
	uint8_t type;
};

// What a monitor reports about the signal it is receiving.
struct timing {
	uint8_t status;      // a set of TIMING_ flags
	uint16_t horizontal; // the line rate, in units of 10 Hz
	uint16_t vertical;   // the refresh rate, in units of 0.01 Hz
};
#define TIMING_OUT_OF_RANGE 0x80        // the signal is outside what the monitor accepts
#define TIMING_UNSTABLE 0x40            // the monitor's own measurement of the signal is not steady
#define TIMING_HORIZONTAL_POSITIVE 0x02 // the horizontal sync signal is positive
#define TIMING_VERTICAL_POSITIVE 0x01   // the vertical sync signal is positive

// Each builder writes a complete packet to `out`, which must hold DDC_MAX_PACKET bytes, and
// returns the packet's length.
size_t ddc_get_request(uint8_t code, uint8_t *out);
size_t ddc_set_request(uint8_t code, uint16_t value, uint8_t *out);
size_t ddc_caps_request(uint16_t offset, uint8_t *out);
size_t ddc_timing_request(uint8_t *out);

// Reads the reply to a get request for `code`.
// Returns 0 and fills `out` on success, 1 when the monitor says it does not support the code,
// and -1 when the bytes are not a valid reply for this code.
int ddc_parse_vcp_reply(const uint8_t *reply, size_t len, uint8_t code, struct vcp_value *out);

// Reads the reply to a capability request made at `offset`.
// Returns the number of text bytes and points `text` at them. 0 means the string has ended.
// Returns -1 when the bytes are not a valid reply for this offset.
int ddc_parse_caps_reply(const uint8_t *reply, size_t len, uint16_t offset, const uint8_t **text);

// Says whether a reply is a null message: a valid DDC/CI reply of length zero. A monitor sends
// one when it has no answer ready for the request it just received. Bytes after the first
// three are whatever was on the wire, and are ignored. Returns 1 or 0.
int ddc_is_null_reply(const uint8_t *reply, size_t len);

// Reads the reply to a timing request.
// Returns 0 and fills `out` on success, and -1 when the bytes are not a valid timing reply.
int ddc_parse_timing_reply(const uint8_t *reply, size_t len, struct timing *out);

#endif
