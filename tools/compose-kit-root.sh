#!/usr/bin/env bash
# Build a composed root that links OpenGL and compiles our app.
#
# Neither upstream driver does this alone:
#
#   ps5-native-app-boilerplate  takes APP_SOURCE_DIR/APP_PARAM/APP_SCE_SYS,
#                               but has no import-stub support, and OpenGL
#                               needs libSceAgc.so and libSceAgcDriver.so
#                               passed to the linker.
#   ps5-homebrew-ui             links OpenGL correctly via APP_IMPORT_STUBS,
#                               but hardcodes its own src/ and sce_sys/.
#
# So the kit is the driver, and this script gives it a root whose src/ holds
# the kit's modules plus Ghostcontrol's app, and whose sce_sys/ is ours. The
# kit's own demo shell (main.cpp, app/, concepts/, demo/) is left out: it is
# a design gallery, not our application.
#
# Everything heavy is shared with the clone by symlink -- above all .deps,
# which holds the 215 MB OpenGL SDK.
#
# Usage: compose-kit-root.sh KIT_CLONE APP_DIR OUT_ROOT
set -euo pipefail

kit=$(cd -- "$1" && pwd)
app=$(cd -- "$2" && pwd)
out=$3
sce_sys=${GC_APP_SCE_SYS:-$app/sce_sys}

[[ -d $kit/src/gfx && -d $kit/src/ui ]] || {
    echo "not a ps5-homebrew-ui clone: $kit" >&2; exit 2; }
[[ -d $app/src ]] || { echo "no src/ in $app" >&2; exit 2; }

# The kit modules we take, per its docs/ADOPTING.md "take the kit".
kit_modules=(gfx ui core audio platform runtime third_party)
# Its demo shell, deliberately not taken.
kit_demo=(main.cpp app concepts demo)

mkdir -p "$out"

# Share the clone's top level, except what we replace.
for entry in "$kit"/* "$kit"/.[!.]*; do
    [[ -e $entry ]] || continue
    name=$(basename -- "$entry")
    case $name in
        src|sce_sys|dist|build|assets|runtime|.deps|.git) continue ;;
    esac
    ln -sfn "$entry" "$out/$name"
done

# Generated runtime files and OpenGL link groups belong to this build, not the
# globally shared vendor clone. Native SDK/zlib and pinned SDK payloads stay
# read-only cache links; their bootstrap scripts must already be satisfied.
mkdir -p "$out/runtime" "$out/.deps/ps5-opengl"
cp -a "$kit/runtime/." "$out/runtime/"
for entry in "$kit/.deps"/*; do
    [[ -e $entry ]] || continue
    name=$(basename -- "$entry")
    [[ $name == ps5-opengl ]] && continue
    ln -sfn "$entry" "$out/.deps/$name"
done
for entry in "$kit/.deps/ps5-opengl"/*; do
    [[ -e $entry ]] || continue
    name=$(basename -- "$entry")
    [[ $name == libps5opengl-group.a || $name == current ]] && continue
    ln -sfn "$entry" "$out/.deps/ps5-opengl/$name"
done

# src/ must be a real directory: the kit collects sources with
# `find <root>/src -type f`, which does not descend into a symlink.
rm -rf -- "$out/src"
mkdir -p "$out/src"
for module in "${kit_modules[@]}"; do
    [[ -d $kit/src/$module ]] || continue
    cp -a "$kit/src/$module" "$out/src/$module"
done

# Our application, under its own subdirectory so a kit update cannot collide
# with a file of ours.
cp -a "$app/src" "$out/src/ghostcontrol"
# OpenProspero is not part of this build. Do not compile its runtime stub.
rm -f "$out/src/ghostcontrol/openprospero_runtime.c"
# The only core code admitted into the owning app is the pure bounded pad wire
# contract. Never copy gc_main, remote readers, bridge code or privilege helpers.
for contract in gc_app_pad.h gc_app_pad.c; do
    [[ -f $app/../core/src/$contract ]] || {
        echo "missing app-pad contract: $contract" >&2; exit 2; }
    cp "$app/../core/src/$contract" "$out/src/ghostcontrol/$contract"
done

# Guard: the demo shell must not have come along, or its main() would compete
# with ours and the 21 demo designs would be compiled into the app.
for leftover in "${kit_demo[@]}"; do
    [[ ! -e $out/src/$leftover ]] || {
        echo "demo shell leaked into the composed root: src/$leftover" >&2
        exit 2
    }
done
# Exactly one main() is expected: ours.
mains=$(grep -rlE '^[[:space:]]*int[[:space:]]+main[[:space:]]*\(' \
    "$out/src" --include='*.c' --include='*.cpp' --include='*.cc' | wc -l)
(( mains == 1 )) || {
    echo "expected exactly one main() in the composed root, found $mains" >&2
    exit 2
}

# Presentation files are ours. The Shell will refuse to launch a title whose
# sce_sys is incomplete -- icon0.png alone is not enough, it also wants
# pic0.dds, pic1.dds and snd0.at9 -- so this is checked here rather than
# discovered as "cannot start the game or app" on the console.
rm -rf -- "$out/sce_sys"
cp -a "$sce_sys" "$out/sce_sys"
for required in icon0.png param.json pic0.dds pic1.dds snd0.at9; do
    [[ -f $sce_sys/$required ]] || {
        echo "sce_sys/$required is missing; the Shell will refuse to launch" >&2
        exit 2
    }
done

# Packaged assets: the kit's baked fonts and sound bank, which its own code
# loads from /app0/assets, with ours overlaid on top so we can add or replace
# any of them without editing the clone.
rm -rf -- "$out/assets"
mkdir -p "$out/assets"
[[ ! -d $kit/assets ]] || cp -a "$kit/assets/." "$out/assets/"
[[ ! -d $app/assets ]] || cp -a "$app/assets/." "$out/assets/"
# Never remove historical product assets from source. A diagnostic build
# excludes only the stale legacy companion pair in its private composition.
if [[ ${GC_BUILD_VARIANT:-resident} == diagnostic ]]; then
    rm -f "$out/assets/ghostcontrol/ghostcontrol-app-service.elf" \
          "$out/assets/ghostcontrol/ghostcontrol-app-service.sha256"
fi
if [[ -n ${GC_COMPANION_ASSETS:-} ]]; then
    mkdir -p "$out/assets/ghostcontrol"
    cp -a "$GC_COMPANION_ASSETS/." "$out/assets/ghostcontrol/"
fi

printf 'composed root: %s\n' "$out"
printf '  kit modules: %s\n' "${kit_modules[*]}"
printf '  app sources: src/ghostcontrol\n'
