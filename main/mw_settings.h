/*
 * Motorwise read-only reader: the settings, built in (no Wi-Fi, no web configuration).
 * Licence: GPL-3.0, like the rest of wican-fw.
 */
#ifndef MW_SETTINGS_H
#define MW_SETTINGS_H

/* What AT@1 answers, so an app can tell this reader from a stock WiCAN. ATI and ATZ answer as before. */
#define MW_FW_VERSION          "1"
#define MW_DEVICE_DESCRIPTION  "Motorwise read-only v" MW_FW_VERSION " (WiCAN)"

/* The car side: ELM327 on 11-bit CAN at 500 kbit/s, acknowledging frames (needed to ask for readings). */
#define MW_CAN_RATE            CAN_500K

/* Sleep: below this voltage for MW_SLEEP_MINUTES and no OBD request for as long; wakes on voltage or CAN traffic
 * (sleep_mode.c). 13.1 V and 3 minutes are the stock defaults the units ran with since 13 Nov 2026. */
#define MW_SLEEP_VOLTAGE       13.1f
#define MW_SLEEP_MINUTES       3

/* The Bluetooth passkey lives in NVS, written per unit at flash time (tools/flash_unit.sh), never in the image. */
#define MW_NVS_NAMESPACE       "motorwise"
#define MW_NVS_PASSKEY_KEY     "ble_passkey"

#endif
