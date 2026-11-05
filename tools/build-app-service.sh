#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Explicit compile/test candidate only. No deploy, loader, title-ID, or takeover.
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${GC_APP_SERVICE_BUILD_ROOT:-$repo/.build/app-service}"
port="${GC_APP_SERVICE_PORT:-39073}"
[[ "$port" =~ ^[1-9][0-9]{3,4}$ && "$port" -ge 1024 && "$port" -le 65535 ]] || { printf 'invalid service port\n' >&2; exit 2; }
case "${GC_STRICT_ZERO_KERNEL_IMPORTS:-0}" in 0|1) ;; *) printf 'strict audit flag must be 0 or 1\n' >&2; exit 2;; esac
mkdir -p "$out"
ulimit -c 0
sources=(gc_app_service gc_runtime_commands gc_app_pad gc_config gc_profile gc_ini gc_ipc gc_log_tail gc_controller_commands gc_service_audio gc_service_capture audio/gc_audio audio/gc_audio_capture audio/gc_audio_settings audio/gc_audio_dsp gc_file_safety)
src=(); for f in "${sources[@]}"; do src+=("$repo/core/src/$f.c"); done
variant="${2:-resident}"
case "$variant" in resident|resident-no-capture|diagnostic) ;; *) printf 'variant must be resident, resident-no-capture, or diagnostic\n' >&2; exit 2;; esac
variant_flags=()
if [[ "$variant" == diagnostic ]]; then variant_flags=(-DGC_APP_SERVICE_DIAGNOSTIC=1); fi
if [[ "$variant" == resident-no-capture ]]; then variant_flags=(-DGC_APP_SERVICE_NO_AUDIO_CAPTURE=1); fi
common=("${variant_flags[@]}" -std=c11 -D_DEFAULT_SOURCE= -Wall -Wextra -Wshadow -Werror -DGC_APP_SERVICE_PORT="$port" -I"$repo/core/src")
mode="${1:-all}"
case "$mode" in host|native|all) ;; *) printf 'usage: %s [host|native|all] [resident|resident-no-capture|diagnostic]\n' "$0" >&2; exit 2;; esac
if [[ "$mode" != native ]]; then
    clang "${common[@]}" -pthread -DGC_APP_SERVICE_NO_MAIN -I"$repo/ui/src" \
      "$repo/core/tests/test_gc_app_service.c" "${src[@]}" "$repo/ui/src/gc_client.c" \
      -ljson-c -lm -o "$out/test-app-service"
    "$out/test-app-service" | tee "$out/host-integration.log"
    capture_test_out="$out/capture-policy"
    mkdir -p "$capture_test_out"
    clang "${common[@]}" -pthread -DGC_APP_SERVICE_NO_MAIN \
      -DGC_APP_SERVICE_NO_AUDIO_CAPTURE_TEST -I"$repo/ui/src" \
      "$repo/core/tests/test_gc_app_service.c" "${src[@]}" "$repo/ui/src/gc_client.c" \
      -Wl,--wrap=gc_service_capture_attach -ljson-c -lm -o "$capture_test_out/test-app-service-capture-policy"
    GC_APP_SERVICE_BUILD_ROOT="$capture_test_out" "$capture_test_out/test-app-service-capture-policy" \
      2>&1 | tee "$capture_test_out/host-capture-policy.log"
    clang "${common[@]}" -pthread "${src[@]}" -ljson-c -lm -o "$out/ghostcontrol-app-service.host"
    clang "${common[@]}" -pthread -DGC_APP_SERVICE_NO_MAIN -I"$repo/ui/src" \
      -fsanitize=address,undefined -fno-omit-frame-pointer -g \
      "$repo/core/tests/test_gc_app_service.c" "${src[@]}" "$repo/ui/src/gc_client.c" \
      -ljson-c -lm -o "$out/test-app-service-sanitized"
    "$out/test-app-service-sanitized" | tee "$out/host-sanitized.log"
    # Explicit host-only variants: actual flock acquisition is linker-wrapped.
    # Production/default binaries above contain neither macro nor wrap option.
    for test_variant in normal sanitized; do
        fault_out="$out/ownership-$test_variant"
        mkdir -p "$fault_out"
        sanitizer=()
        if [[ "$test_variant" == sanitized ]]; then
            sanitizer=(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
        fi
        clang "${common[@]}" -pthread -DGC_APP_SERVICE_NO_MAIN \
          -DGC_APP_SERVICE_TEST_FLOCK -I"$repo/ui/src" "${sanitizer[@]}" \
          "$repo/core/tests/test_gc_app_service.c" "${src[@]}" "$repo/ui/src/gc_client.c" \
          -Wl,--wrap=flock -ljson-c -lm -o "$fault_out/test-app-service"
        GC_APP_SERVICE_BUILD_ROOT="$fault_out" "$fault_out/test-app-service" \
          2>&1 | tee "$fault_out/host-ownership.log"
        nm "$fault_out/test-app-service" > "$fault_out/symbols.txt"
    done
    nm "$out/ghostcontrol-app-service.host" > "$out/host-symbols.txt"
    nm "$out/test-app-service" > "$out/host-test-symbols.txt"
    nm "$out/test-app-service-sanitized" > "$out/host-sanitized-symbols.txt"
    python3 - "$out" <<'PY'
import pathlib, sys
out = pathlib.Path(sys.argv[1])
for name in ('host-symbols.txt', 'host-test-symbols.txt', 'host-sanitized-symbols.txt'):
    text = (out / name).read_text()
    assert not any(symbol in text for symbol in
                   ('__wrap_flock', '__real_flock', 'flock_fault', 'ownership_failures'))
    symbols = [line.split() for line in text.splitlines() if line.split()]
    assert any(fields[-2] == 'U' and fields[-1].split('@')[0] == 'flock'
               for fields in symbols), 'real flock import missing'
for variant in ('normal', 'sanitized'):
    text = (out / ('ownership-' + variant) / 'symbols.txt').read_text()
    assert '__wrap_flock' in text and 'flock_fault_calls' in text
print('default host companion and tests: real flock; fault hooks absent PASS')
print('explicit host ownership variants: actual flock wrapper present PASS')
PY
    # Ordinary HOST only: actual default standalone CLI, private preload fixture
    # at real OS readiness boundaries plus 200 entirely unpreloaded attempts.
    if [[ "$mode" == host ]]; then
        python3 "$repo/tools/test-app-service-lifecycle.py" \
          "$out/ghostcontrol-app-service.host" "$out/lifecycle" \
          --tmpdir "${TMPDIR:?explicit existing host TMPDIR required}" \
          2>&1 | tee "$out/host-lifecycle.log"
    fi
fi
if [[ "$mode" != host ]]; then
    : "${PS5_PAYLOAD_SDK:?stock payload SDK required}"
    : "${GC_PORTS_ROOT:?cached ports root required}"
    make -C "$repo/GhosttoothAPI" all VARIANT="$variant" PORT="$port" PORTS="$GC_PORTS_ROOT" \
      ELF="$out/GhosttoothAPI.elf"
    audit_profile=ordinary
    if [[ "$variant" == diagnostic ]]; then audit_profile=diagnostic; fi
    python3 "$repo/tools/audit-companion-imports.py" --elf "$out/GhosttoothAPI.elf" \
      --sdk "$PS5_PAYLOAD_SDK" --profile "$audit_profile" --source-root "$repo" \
      --report "$out/native-import-audit.json"
    if [[ "${GC_STRICT_ZERO_KERNEL_IMPORTS:-0}" == 1 ]]; then
        printf 'STRICT zero-kernel-library gate: FAIL (ordinary POSIX kernel provider present)\n' >&2
        exit 1
    fi
fi
