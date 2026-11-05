#!/usr/bin/env bash
# Fetch the upstream pieces this tree builds against.
#
# They are cloned rather than vendored so this repository holds only
# Ghostcontrol's own source, and so upstream fixes arrive with a pull instead
# of a re-import. Both are pinned by tag or commit in deps.lock when one is
# recorded there; otherwise the default branch is taken and the resolved
# commit is written back so a later build can be reproduced.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
# Where the clones go. Override with GC_VENDOR_DIR when the checkout lives
# somewhere that cannot delete files -- the upstream build needs to remove
# scratch files, so it cannot run under a read-only or no-unlink mount.
vendor="${GC_VENDOR_DIR:-$root/ui/vendor}"
lock="$root/tools/deps.lock"

declare -A repos=(
  [ps5-native-app-boilerplate]=https://github.com/blackbearreloaded/ps5-native-app-boilerplate.git
  [ps5-homebrew-ui]=https://github.com/blackbearreloaded/ps5-homebrew-ui.git
  [ps5-runtime-shim]=https://github.com/blackbearreloaded/ps5-native-app-boilerplate.git
)

mkdir -p "$vendor"
: > "$lock.new"

for name in "${!repos[@]}"; do
    url=${repos[$name]}
    target="$vendor/$name"
    pinned=""
    if [[ -f "$lock" ]]; then
        pinned=$(awk -v n="$name" '$1 == n { print $2 }' "$lock" || true)
    fi

    if [[ -d "$target/.git" ]]; then
        echo "==> $name: updating"
        git -C "$target" fetch --quiet --depth 1 origin "${pinned:-HEAD}"
        git -C "$target" checkout --quiet FETCH_HEAD
    else
        echo "==> $name: cloning"
        if [[ -n "$pinned" ]]; then
            git init --quiet "$target"
            git -C "$target" remote add origin "$url"
            git -C "$target" fetch --quiet --depth 1 origin "$pinned"
            git -C "$target" checkout --quiet FETCH_HEAD
        else
            git clone --quiet --depth 1 "$url" "$target"
        fi
    fi

    resolved=$(git -C "$target" rev-parse HEAD)
    printf '%s %s\n' "$name" "$resolved" >> "$lock.new"
    echo "    $resolved"
done

mv "$lock.new" "$lock"
echo "==> pinned commits recorded in tools/deps.lock"
