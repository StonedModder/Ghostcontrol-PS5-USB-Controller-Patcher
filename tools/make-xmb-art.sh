#!/usr/bin/env bash
# Rebuild the PS5 sce_sys background art (pic0/pic1) as 4K BC7 DDS.
#
#   tools/make-xmb-art.sh [OUT_DIR]
#
# Requires: python3 with numpy + pillow + opencv, and texconv (DirectXTex).
# Set TEXCONV to the texconv binary if it is not on PATH.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(dirname "$here")"
out="${1:-$root/ui/sce_sys}"

python3 "$here/make-xmb-art.py" --out "$out" --preview "$out/preview"

texconv="${TEXCONV:-$(command -v texconv || true)}"
if [ -z "$texconv" ]; then
  echo "texconv not found; set TEXCONV=/path/to/texconv" >&2
  exit 1
fi

for name in pic0 pic1; do
  "$texconv" -y -f BC7_UNORM -dx10 -m 1 -o "$out" "$out/preview/$name.png" >/dev/null
  echo "wrote $out/$name.dds"
done

python3 - "$out" <<'PY'
import struct, sys, pathlib
d = pathlib.Path(sys.argv[1])
for n in ("pic0.dds", "pic1.dds"):
    b = (d / n).read_bytes()
    w, h = struct.unpack('<2I', b[16:24])[::-1]
    mips = struct.unpack('<I', b[28:32])[0]
    dxgi = struct.unpack('<I', b[128:132])[0]
    assert b[:4] == b'DDS ' and dxgi == 98 and mips == 1, (n, b[:4], dxgi, mips)
    assert len(b) - 148 == w * h, (n, len(b), w, h)
    print(f"{n}: {w}x{h} BC7_UNORM mips={mips} bytes={len(b)}")
PY
