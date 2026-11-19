# Motorwise read-only reader (fork of wican-fw, branch `read-only`)

A WiCAN-OBD-C3 firmware that only reads the car. Built with `CONFIG_MOTORWISE_BLE_ONLY=y` (the default in this
branch, menu "Motorwise" in menuconfig).

## What it is

- **Bluetooth LE only.** ELM327 over BLE: service `fee0`, commands written to `fee1`, answers notified on `fee1`.
  No access point, no web configuration server, no Wi-Fi firmware updates, no MQTT, webhooks, FTP or TCP port:
  those sources are not compiled (`main/CMakeLists.txt`); `main/mw_main.c` replaces `main/main.c`.
- **Read-only gate** (`main/read_only.c`, PC test `test/read_only_test.c`): every frame for the car passes
  `can_send()`, which only lets reading requests (services 01 03 07 09 0A 19 21 22) and flow control through.
- **Settings built in** (`main/mw_settings.h`): ELM327 on CAN 500 kbit/s, BLE on, sleep on. Sleep: below 13.1 V for
  3 minutes *and* no OBD request for 3 minutes; while asleep it wakes on voltage or on CAN traffic (checked every
  2 s, listen-only). Bluetooth is off while asleep. Wake is a restart.
- **Passkey per unit, in NVS** (namespace `motorwise`, key `ble_passkey`, u32), written over USB at flash time, never
  in the image. No passkey in NVS: Bluetooth stays off and the log says `no passkey`. There is no default passkey.
- **Bonds** are stored in NVS by the Bluetooth stack (`CONFIG_BT_BLE_SMP_BOND_NVS_FLASH=y`); nothing in the firmware
  erases NVS or removes bonds.
- **Identity:** `AT@1` answers `Motorwise read-only v1 (WiCAN)`. `ATI` (`OBDLink MX`) and `ATZ` (`ELM327 v1.3a`)
  answer as in the stock firmware. The BLE name stays `WiC_<access-point MAC>`.

## Flashing a unit (`tools/flash_unit.sh`)

Build first: `. ~/esp/esp-idf-v5.5.2/export.sh && idf.py build`.

| | `first <port> <secrets-file>` | `update <port>` |
|---|---|---|
| When | a new unit, or one coming from the stock or a Wi-Fi build | a firmware update of a Motorwise unit |
| Writes | bootloader, partition table, otadata, app (ota_0), a fresh NVS with the passkey | otadata, app (ota_0) |
| Erases | the second app slot (ota_1), so the bootloader can't fall back to a stock Wi-Fi firmware | nothing |
| NVS | replaced: **all bonds are forgotten, phones pair again** | never touched: passkey and bonds kept |
| Checks | the secrets file's MAC (if its name has one) matches the chip; passkey is 6 digits | NVS holds a Motorwise passkey, else it refuses |
| Verify | `verify_flash` of every written region (MD5 on the chip) | same |

The secrets file has a line `ble_pass=NNNNNN` (mode 600). The script reads it without printing it; the generated
NVS image lives in a temporary folder and is shredded at exit. The storage partition (stock `config.json`) is left
as it is; this firmware doesn't read it.

Bench notes: asleep, the unit has no USB port (it sleeps 3 minutes after start-up on USB power, since USB gives no
12 V): replug it and run the script within those minutes. Opening the port with changed DTR/RTS resets the chip.

## Bench test over Bluetooth (`tools/ble_bench.py`)

Uses the laptop's own adapter through BlueZ (python3-dbus, python3-gi; nothing to install on Ubuntu).

- `pair <name> <secrets-file>`: pair and bond, the passkey typed from the file; then sends `AT@1` and `ATI`.
- `check <name>`: connect on the existing bond and ask again. Its agent refuses any passkey request, so a reader
  that lost the bond fails the check ("bond NOT kept").
- `remove <name>`: forget the bond on the laptop.

Bond test: `pair`, then `check` after (a) a reset over USB, (b) sleep and wake, (c) `flash_unit.sh update`.
