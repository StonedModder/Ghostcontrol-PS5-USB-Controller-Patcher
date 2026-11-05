#!/bin/sh
# check_imports.sh ELF [GOLDEN]
#
# Ported from HearBridge-PS5 scripts/check_imports.sh (GPL-3.0-or-later), same
# contract and same reason: the ps5-payload-sdk runtime loads every DT_NEEDED
# module and binds every imported symbol BEFORE main() runs, and any failure
# there ends the payload silently - no notification, no log, nothing on 9021.
# A payload that never appears is more likely to be this than anything else.
#
#   * DT_NEEDED must be an ordered subsequence of the golden list (load order);
#   * every undefined dynamic symbol must be in the golden symbol list.
# Anything new has to be resolved at run time instead, e.g.
#   kernel_dynlib_handle(-1, "libSceLibcInternal.sprx", &h) + kernel_dynlib_dlsym.
#
# Usage:  tools/check_imports.sh out/ghostcontrol-core-<ver>.elf
#         tools/check_imports.sh <elf> tools/imports-golden.txt
set -eu

elf=${1:?usage: check_imports.sh ELF [GOLDEN]}
allow=${2:-$(dirname "$0")/imports-golden.txt}
[ -r "$elf" ] || { echo "check_imports: cannot read $elf" >&2; exit 2; }
[ -r "$allow" ] || { echo "check_imports: cannot read $allow" >&2; exit 2; }

READELF=${READELF:-readelf}
command -v "$READELF" >/dev/null 2>&1 || READELF=llvm-readelf
command -v "$READELF" >/dev/null 2>&1 || READELF=/usr/lib/llvm-18/bin/llvm-readelf
command -v "$READELF" >/dev/null 2>&1 || {
    echo "check_imports: no readelf (set READELF=)" >&2; exit 2; }

tmp=${TMPDIR:-/tmp}/gc_imports.$$
trap 'rm -f "$tmp".*' EXIT INT TERM

sed -n 's/^needed //p' "$allow" > "$tmp.need_ok"
grep -v '^#' "$allow" | grep -v '^needed ' | sed '/^$/d' | sort -u > "$tmp.sym_ok"
"$READELF" -d "$elf" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' > "$tmp.need"
"$READELF" --dyn-syms -W "$elf" |
    awk '$7 == "UND" && $8 != "" { sub(/@.*/, "", $8); print $8 }' | sort -u > "$tmp.sym"

status=0
if ! awk 'NR == FNR { ok[++n] = $0; next }
          { while (i < n && ok[++i] != $0) ; if (ok[i] != $0) bad = 1 }
          END { exit bad }' "$tmp.need_ok" "$tmp.need"; then
    echo "check_imports: DT_NEEDED is not an ordered subsequence of the golden list:" >&2
    diff "$tmp.need_ok" "$tmp.need" >&2 || true
    status=1
fi
new=$(comm -13 "$tmp.sym_ok" "$tmp.sym")
if [ -n "$new" ]; then
    echo "check_imports: imports the golden list does not have (resolve them at run time):" >&2
    echo "$new" | sed 's/^/  /' >&2
    status=1
fi
[ $status -eq 0 ] &&
    echo "check_imports: $(wc -l < "$tmp.sym") imports, NEEDED $(tr '\n' ' ' < "$tmp.need")within the golden list: ok"
exit $status
