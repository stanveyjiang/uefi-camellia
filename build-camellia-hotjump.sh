#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CONFIG="$ROOT/Resources/Configs/camellia.toml"
BACKUP=$(mktemp "$ROOT/.camellia.toml.XXXXXX")
DSC="$ROOT/Platforms/Xiaomi/camelliaPkg/camellia.dsc"
DSC_BACKUP=$(mktemp "$ROOT/.camellia.dsc.XXXXXX")
FD="$ROOT/Build/camelliaPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd"
OUTDIR="$ROOT/dist"
OUTPUT="$OUTDIR/SILICIUM_UEFI-hotjump.fd"

cleanup() {
	cp "$BACKUP" "$CONFIG"
	cp "$DSC_BACKUP" "$DSC"
	find "$BACKUP" -maxdepth 0 -delete
	find "$DSC_BACKUP" -maxdepth 0 -delete
}
trap cleanup EXIT

cp "$CONFIG" "$BACKUP"
cp "$DSC" "$DSC_BACKUP"
sed -i 's/^base = 0x40080000$/base = 0xA0000000/' "$CONFIG"
sed -i 's/^  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x40000000$/  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0xA0200000/' "$DSC"
grep -q '^base = 0xA0000000$' "$CONFIG" || {
	echo "failed to select the hot-jump FD base" >&2
	exit 1
}
grep -q 'PcdCPUCoresStackBase|0xA0200000' "$DSC" || {
	echo "failed to select the hot-jump UEFI stack" >&2
	exit 1
}

cd "$ROOT"
python3 build_uefi.py -d camellia -r DEBUG -c "$@"

[[ -f "$FD" ]] || {
	echo "UEFI build did not produce $FD" >&2
	exit 1
}
[[ $(stat -c %s "$FD") -eq 2097152 ]] || {
	echo "unexpected FD size: $(stat -c %s "$FD")" >&2
	exit 1
}

mkdir -p "$OUTDIR"
cp "$FD" "$OUTPUT"

entry=$(python3 - "$OUTPUT" <<'PY'
from pathlib import Path
import sys
blob = Path(sys.argv[1]).read_bytes()
print(f"0x{int.from_bytes(blob[0x1098:0x10a0], 'little'):x}")
PY
)
python3 - "$entry" <<'PY'
import sys
entry = int(sys.argv[1], 16)
if not 0xA0000000 <= entry < 0xA0200000:
    raise SystemExit(f"not a hot-jump FD: C entry {entry:#x}")
print(f"hot-jump C entry: {entry:#x}")
PY
python3 - "$OUTPUT" <<'PY'
from pathlib import Path
import sys

blob = Path(sys.argv[1]).read_bytes()
expected = bytes.fromhex("0000e0d20000c0f20004b4f2000080f2")
actual = blob[0x1058:0x1068]
if actual != expected:
    raise SystemExit(
        "unexpected hot-jump SEC stack setup: "
        f"expected {expected.hex()}, got {actual.hex()}")
print("hot-jump SEC stack setup verified")
PY

sha256sum "$OUTPUT"
