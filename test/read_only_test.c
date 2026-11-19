/* PC test of the read-only gate: gcc -Wall -Wextra -I../main ../main/read_only.c read_only_test.c && ./a.out */
#include <stdio.h>
#include <string.h>

#include "read_only.h"

static int failures = 0, checks = 0;

static void expect(bool allowed, uint32_t id, bool ext, const char *hex)
{
	uint8_t d[8];
	memset(d, 0xAA, 8);
	size_t n = strlen(hex) / 2;
	if (n > 8) n = 8;
	for (size_t i = 0; i < n; i++) sscanf(hex + 2 * i, "%2hhx", &d[i]);
	const char *why = read_only_check(id, ext, false, 8, d);
	checks++;
	if ((why == NULL) != allowed)
	{
		failures++;
		printf("FAIL %s %08X %s: %s\n", allowed ? "should pass" : "should be refused", id, hex, why ? why : "passed");
	}
}

#define PASS(id, hex) expect(true, id, false, hex)
#define STOP(id, hex) expect(false, id, false, hex)

int main(void)
{
	/* Reads pass. */
	PASS(0x7DF, "020100"); PASS(0x7E0, "03010C0D"); PASS(0x7DF, "020902"); PASS(0x7E0, "0322F190");
	PASS(0x7E0, "0722F190F18CF187"); PASS(0x7DF, "0103"); PASS(0x7DF, "0107"); PASS(0x7DF, "010A");
	PASS(0x7E0, "031902FF"); PASS(0x7E0, "022101"); PASS(0x6F1 + 0x100, "0322DDBC");
	expect(true, 0x18DB33F1, true, "020100"); expect(true, 0x18DA10F1, true, "0322F190");

	/* Flow control for a long answer passes, with any timing. */
	PASS(0x7E0, "300000"); PASS(0x7E0, "30000A"); PASS(0x7E0, "300800"); PASS(0x7E0, "310000");

	/* Every service byte: only the reading ones pass (with a valid argument each). */
	for (int s = 0; s < 256; s++)
	{
		char hex[16];
		bool reading = s == 0x01 || s == 0x03 || s == 0x07 || s == 0x09 || s == 0x0A || s == 0x19 || s == 0x21 || s == 0x22;
		if (s == 0x03 || s == 0x07 || s == 0x0A) snprintf(hex, sizeof hex, "01%02X", s);
		else if (s == 0x22) snprintf(hex, sizeof hex, "03%02XF190", s);
		else if (s == 0x19) snprintf(hex, sizeof hex, "03%02X02FF", s);
		else snprintf(hex, sizeof hex, "02%02X01", s);
		expect(reading, 0x7E0, false, hex);
	}

	/* The dangerous ones by name: clear codes, session, reset, security, write, output control, routine,
	   tester present, write memory, clear DTC (UDS), control DTC setting, communication control. */
	STOP(0x7DF, "0104"); STOP(0x7E0, "021003"); STOP(0x7E0, "021101"); STOP(0x7E0, "022701");
	STOP(0x7E0, "042EF19001"); STOP(0x7E0, "042F100103"); STOP(0x7E0, "043101FF00"); STOP(0x7E0, "023E00");
	STOP(0x7E0, "013D"); STOP(0x7E0, "0414FFFFFF"); STOP(0x7E0, "028502"); STOP(0x7E0, "032803");
	STOP(0x7E0, "020801"); STOP(0x7E0, "0219FF");

	/* Malformed: wrong lengths, multi-frame requests, bad flow control. */
	STOP(0x7E0, "0222F1"); STOP(0x7E0, "0422F190F1"); STOP(0x7E0, "0203FF"); STOP(0x7E0, "032101FF"); STOP(0x7E0, "0101");
	STOP(0x7E0, "00"); STOP(0x7E0, "0822F190F18CF187"); STOP(0x7E0, "100A2EF190010203"); STOP(0x7E0, "2101020304050607");
	STOP(0x7E0, "330000"); STOP(0x7E0, "3F0000");

	/* Not a diagnostic address: the car's own traffic is never imitated. */
	STOP(0x244, "020100"); STOP(0x000, "020100"); STOP(0x6FF, "020100"); STOP(0x800, "020100");
	expect(false, 0x18FEF100, true, "020100"); expect(false, 0x18DA10F2, true, "020100"); expect(false, 0x7E0, true, "020100");

	/* Remote frames and empty frames. */
	{
		uint8_t d[8] = {0x02, 0x01, 0x00};
		checks += 3;
		if (read_only_check(0x7DF, false, true, 8, d) == NULL) { failures++; puts("FAIL remote frame passed"); }
		if (read_only_check(0x7DF, false, false, 0, d) == NULL) { failures++; puts("FAIL empty frame passed"); }
		if (read_only_check(0x7DF, false, false, 2, d) == NULL) { failures++; puts("FAIL frame shorter than its length passed"); }
	}

	printf("%d checks, %d failures\n", checks, failures);
	return failures != 0;
}
