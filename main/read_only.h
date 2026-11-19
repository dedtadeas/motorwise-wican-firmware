/*
 * Read-only gate (Motorwise fork). Every frame WiCAN puts on the car's bus passes read_only_allows() first,
 * whichever protocol asked for it (ELM327, slcan, SavvyCAN, RealDash, MQTT, AutoPID).
 *
 * Allowed: a single-frame request for a reading service (01, 03, 07, 09, 0A, 19, 21, 22) to a diagnostic address,
 * and the flow-control frame that asks an ECU for the rest of a long answer. Everything else is refused:
 * sessions, resets, security access, writing, routines, clearing codes, tester present, multi-frame requests,
 * remote frames and frames to any other address.
 *
 * Plain C with no ESP-IDF headers, so the same file is tested on a PC (test/read_only_test.c).
 * Licence: GPL-3.0, like the rest of wican-fw.
 */
#ifndef READ_ONLY_H
#define READ_ONLY_H

#include <stdbool.h>
#include <stdint.h>

/* Returns NULL if the frame may be sent, or a short reason it may not. */
const char *read_only_check(uint32_t id, bool extended, bool remote, uint8_t dlc, const uint8_t *data);

static inline bool read_only_allows(uint32_t id, bool extended, bool remote, uint8_t dlc, const uint8_t *data)
{
	return read_only_check(id, extended, remote, dlc, data) == 0;
}

#endif
