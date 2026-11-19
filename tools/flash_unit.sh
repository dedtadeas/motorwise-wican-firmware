#!/usr/bin/env bash
# Flash a WiCAN-OBD-C3 with the Motorwise read-only firmware over USB.
#
#   tools/flash_unit.sh first  <port> <secrets-file>   new unit: whole flash layout plus a fresh NVS with its passkey
#   tools/flash_unit.sh update <port>                  firmware update: app and otadata only, NVS never touched
#
# <secrets-file> holds a line "ble_pass=NNNNNN" (6 digits). Its name may carry the unit's MAC
# (e.g. unit-58e6c5984260-secrets.txt); if it does, it must match the chip, so one unit's passkey can't land on
# another. The passkey is read inside this script only: never printed, never kept on disk after the run.
#
# "first" writes a fresh NVS, so it forgets every Bluetooth bond: phones must pair again. "update" keeps NVS
# (passkey and bonds) and refuses to run on a unit whose NVS has no Motorwise passkey.
# Both erase nothing else and verify every written region against the files (MD5 on the chip).
#
# Build first: . ~/esp/esp-idf-v5.5.2/export.sh && idf.py build   (CONFIG_MOTORWISE_BLE_ONLY=y)
# Licence: GPL-3.0, like the rest of wican-fw.
set -euo pipefail
umask 077

usage() { sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

MODE=${1:-}; PORT=${2:-}; SECRETS=${3:-}
[[ "$MODE" == first && -n "$PORT" && -n "$SECRETS" ]] || [[ "$MODE" == update && -n "$PORT" ]] || usage

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$REPO/build"
IDF_PATH=${IDF_PATH:-$HOME/esp/esp-idf-v5.5.2}
PY=${IDF_PYTHON:-$HOME/.espressif/python_env/idf5.5_py3.12_env/bin/python}
ESPTOOL=("$PY" -m esptool --chip esp32c3 -p "$PORT" -b 460800)
FLASH_OPTS=(--flash_mode dio --flash_freq 80m --flash_size 4MB)

# The layout of wican_partitions_table.csv.
NVS_OFF=0x9000;   NVS_SIZE=0x4000
OTADATA_OFF=0xd000
APP_OFF=0x10000
OTA1_OFF=0x1d0000; OTA1_SIZE=0x1b3000

BOOTLOADER="$BUILD/bootloader/bootloader.bin"
PARTTABLE="$BUILD/partition_table/partition-table.bin"
OTADATA="$BUILD/ota_data_initial.bin"
APP=$(awk '$1 == "0x10000" {print $2}' "$BUILD/flash_args" 2>/dev/null || true)
APP="$BUILD/$APP"

die() { echo "flash_unit: $*" >&2; exit 1; }

for f in "$BOOTLOADER" "$PARTTABLE" "$OTADATA" "$APP"; do [[ -f "$f" ]] || die "missing $f (build first)"; done
grep -aq "Motorwise read-only v" "$APP" || die "$APP is not a Motorwise read-only build (CONFIG_MOTORWISE_BLE_ONLY)"
[[ -e "$PORT" ]] || die "no $PORT: plug the unit in (asleep it has no USB port; replug wakes it for a few minutes)"

TMP=$(mktemp -d)
cleanup() { [[ -d "$TMP" ]] && { find "$TMP" -type f -exec shred -u {} + 2>/dev/null || true; rm -rf "$TMP"; }; }
trap cleanup EXIT

chip_mac() {
	"${ESPTOOL[@]}" read_mac 2>/dev/null | awk '/^MAC:/ {gsub(":", "", $2); print tolower($2); exit}'
}

echo "== unit"
MAC=$(chip_mac) || true
[[ -n "$MAC" ]] || die "no answer from the chip on $PORT"
echo "chip MAC $MAC"
echo "app $(basename "$APP"), sha256 $(sha256sum "$APP" | cut -c1-16)…"

if [[ "$MODE" == first ]]; then
	[[ -f "$SECRETS" ]] || die "no secrets file $SECRETS"
	FILE_MAC=$(basename "$SECRETS" | grep -oiE '[0-9a-f]{12}' | head -1 | tr 'A-F' 'a-f' || true)
	if [[ -n "$FILE_MAC" && "$FILE_MAC" != "$MAC" ]]; then
		die "the secrets file is for $FILE_MAC, this chip is $MAC"
	fi
	# Read the passkey without echoing it; check its shape only.
	PASS=$(awk -F= '$1 == "ble_pass" {print $2; exit}' "$SECRETS" | tr -d '[:space:]')
	[[ "$PASS" =~ ^[0-9]{6}$ ]] || die "no 6-digit ble_pass line in the secrets file"

	printf 'key,type,encoding,value\nmotorwise,namespace,,\nble_passkey,data,u32,%s\n' "$((10#$PASS))" > "$TMP/nvs.csv"
	unset PASS
	"$PY" "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" \
		generate "$TMP/nvs.csv" "$TMP/nvs.bin" "$NVS_SIZE" > /dev/null || die "NVS generation failed"
	shred -u "$TMP/nvs.csv"

	echo "== write (bootloader, partition table, otadata, app, fresh NVS with the passkey)"
	"${ESPTOOL[@]}" --after no_reset write_flash "${FLASH_OPTS[@]}" \
		0x0 "$BOOTLOADER" 0x8000 "$PARTTABLE" "$OTADATA_OFF" "$OTADATA" "$APP_OFF" "$APP" "$NVS_OFF" "$TMP/nvs.bin" \
		| grep -E "^(Wrote|Hash|Compressed)" || die "write failed"
	# The second app slot may still hold a stock Wi-Fi firmware the bootloader could fall back to: erase it.
	echo "== erase the second app slot (ota_1)"
	"${ESPTOOL[@]}" --before no_reset --after no_reset erase_region "$OTA1_OFF" "$OTA1_SIZE" | grep -E "^Erase" || die "erase failed"

	echo "== verify"
	"${ESPTOOL[@]}" --before no_reset --after hard_reset verify_flash "${FLASH_OPTS[@]}" \
		0x0 "$BOOTLOADER" 0x8000 "$PARTTABLE" "$OTADATA_OFF" "$OTADATA" "$APP_OFF" "$APP" "$NVS_OFF" "$TMP/nvs.bin" \
		| grep -E "^(Verifying|-- verify OK|Verification)" || die "verify failed"
else
	echo "== check NVS holds a Motorwise passkey (bonds and passkey stay as they are)"
	"${ESPTOOL[@]}" --after no_reset read_flash "$NVS_OFF" "$NVS_SIZE" "$TMP/nvs-read.bin" > /dev/null || die "NVS read failed"
	HAS=$("$PY" -I - "$IDF_PATH/components/nvs_flash/nvs_partition_tool" "$TMP/nvs-read.bin" <<'EOF'
import sys
sys.path.insert(0, sys.argv[1])
import nvs_parser
with open(sys.argv[2], 'rb') as f:
    nvs = nvs_parser.NVS_Partition('nvs', bytearray(f.read()))
ns = {}
found = False
for page in nvs.pages:
    for e in page.entries:
        if e.state == 'Written' and e.metadata['namespace'] == 0 and e.key:
            ns[e.data['value']] = e.key
for page in nvs.pages:
    for e in page.entries:
        if e.state == 'Written' and e.key == 'ble_passkey' and ns.get(e.metadata['namespace']) == 'motorwise':
            found = True
print('yes' if found else 'no')
EOF
)
	shred -u "$TMP/nvs-read.bin"
	[[ "$HAS" == yes ]] || die "this unit's NVS has no Motorwise passkey: use 'first' (it forgets bonds)"
	echo "passkey present"

	echo "== write (otadata, app)"
	"${ESPTOOL[@]}" --before no_reset --after no_reset write_flash "${FLASH_OPTS[@]}" \
		"$OTADATA_OFF" "$OTADATA" "$APP_OFF" "$APP" \
		| grep -E "^(Wrote|Hash|Compressed)" || die "write failed"
	echo "== verify"
	"${ESPTOOL[@]}" --before no_reset --after hard_reset verify_flash "${FLASH_OPTS[@]}" \
		"$OTADATA_OFF" "$OTADATA" "$APP_OFF" "$APP" \
		| grep -E "^(Verifying|-- verify OK|Verification)" || die "verify failed"
fi
echo "== done: $MODE on $MAC, the unit restarts now"
