#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
if [[ -d /usr/lib/llvm-18/bin ]]; then
	export PATH=/usr/lib/llvm-18/bin:$PATH
fi
clang --version | grep -q 'version 18\.' || {
	echo "clang 18 is required for the reproducible Camellia build" >&2
	exit 1
}

FD="$ROOT/Build/camelliaPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd"
OUTDIR="$ROOT/dist"
OUTPUT="$OUTDIR/SILICIUM_UEFI-coldboot.fd"

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
if not 0x40080000 <= entry < 0x40280000:
    raise SystemExit(f"not a cold-boot FD: C entry {entry:#x}")
print(f"cold-boot C entry: {entry:#x}")
PY

sha256sum "$OUTPUT"
