/* Read-only gate (Motorwise fork): see read_only.h. GPL-3.0. */
#include "read_only.h"

#include <stddef.h>

/* 11-bit: the OBD broadcast 7DF and the diagnostic range 700–7FF (ECU requests 7E0–7E7 and makers' own addresses). */
static bool diagnostic_11(uint32_t id)
{
	return id >= 0x700 && id <= 0x7FF;
}

/* 29-bit OBD (ISO 15765-4): broadcast 18DB33F1 and physical 18DAxxF1, tester F1. */
static bool diagnostic_29(uint32_t id)
{
	return id == 0x18DB33F1 || (id & 0xFFFF00FF) == 0x18DA00F1;
}

/* Request length in bytes (service byte included) that each reading service accepts. */
static bool length_ok(uint8_t service, uint8_t len)
{
	switch (service)
	{
		case 0x01: case 0x09:   /* current data, vehicle info: up to 6 IDs */
			return len >= 2 && len <= 7;
		case 0x03: case 0x07: case 0x0A:   /* stored, pending, permanent codes: no arguments */
			return len == 1;
		case 0x19:   /* read DTC information: sub-function plus its arguments */
			return len >= 2 && len <= 7;
		case 0x21:   /* read by local ID: one byte */
			return len == 2;
		case 0x22:   /* read by ID: whole two-byte IDs, up to three in one frame */
			return len >= 3 && len <= 7 && (len - 1) % 2 == 0;
		default:
			return false;
	}
}

static bool reading_19(uint8_t sub)
{
	/* ReadDTCInformation is reading by definition, but keep to the sub-functions in common use. */
	switch (sub)
	{
		case 0x01: case 0x02: case 0x03: case 0x04: case 0x06: case 0x0A: case 0x0E: case 0x0F: case 0x13: case 0x14:
		case 0x15: case 0x17: case 0x18: case 0x19: case 0x42: case 0x55:
			return true;
		default:
			return false;
	}
}

const char *read_only_check(uint32_t id, bool extended, bool remote, uint8_t dlc, const uint8_t *data)
{
	if (data == NULL || dlc == 0 || dlc > 8) return "no data";
	if (remote) return "remote frame";
	if (extended ? !diagnostic_29(id) : !diagnostic_11(id)) return "not a diagnostic address";

	uint8_t pci = data[0];
	switch (pci >> 4)
	{
		case 0x3:   /* flow control: continue, wait or overflow; it carries no request */
			if ((pci & 0x0F) > 2) return "bad flow control";
			if (dlc < 3) return "short flow control";
			return NULL;
		case 0x0:
		{
			uint8_t len = pci & 0x0F;
			if (len == 0 || len > 7 || len > dlc - 1) return "bad length";
			uint8_t service = data[1];
			if (!length_ok(service, len)) return "not a reading service";
			if (service == 0x19 && !reading_19(data[2])) return "not a reading sub-function";
			return NULL;
		}
		default:   /* first and consecutive frames: a request longer than seven bytes */
			return "multi-frame request";
	}
}
