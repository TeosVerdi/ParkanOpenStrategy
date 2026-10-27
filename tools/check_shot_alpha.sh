#!/bin/sh
# GPU regression check for atmosphere-render.md section 11 (A9): a --shot frame must be opaque.
# Usage: tools/check_shot_alpha.sh <build/parkan> [<game dir>]   (needs a GPU; not part of parkan-tests)
# Debug env PARKAN_SHOT_KEEP_ALPHA=1 makes --shot keep the raw alpha (normally dropped). Renders C00M02 with --briefing --time 10 and fails if the BMP has any pixel with alpha != 255.
set -e
exe=${1:?usage: check_shot_alpha.sh <parkan binary> [game dir]}
out=${TMPDIR:-/tmp}/shotalpha.$$.bmp
trap 'rm -f "$out"' EXIT
PARKAN_SHOT_KEEP_ALPHA=1 "$exe" --play MISSIONS/CAMPAIGN/CAMPAIGN.00/Mission.02 --briefing --time 10 --shot "$out" ${2:+"$2"} >/dev/null 2>&1 || true # the exit code is not the verdict; the BMP is
[ -s "$out" ] || { echo "FAIL: no screenshot written"; exit 1; }
python3 - "$out" <<'P'
import struct, sys
d = open(sys.argv[1], 'rb').read()
off, = struct.unpack_from('<I', d, 10)
w, h, _, bpp = struct.unpack_from('<iiHH', d, 18)
if bpp != 32:
    print('FAIL: expected a 32 bpp shot (PARKAN_SHOT_KEEP_ALPHA=1), got %d bpp' % bpp); sys.exit(1)
bad = sum(1 for i in range(off + 3, len(d), 4) if d[i] != 255)
print('FAIL: %d pixels with alpha != 255' % bad if bad else 'OK: alpha 255 everywhere')
sys.exit(1 if bad else 0)
P
