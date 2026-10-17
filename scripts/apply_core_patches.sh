#!/usr/bin/env bash
# Applies the patches in patches/ to the SAME_CDI checkouts under third_party/.
# Safe to run repeatedly: patches that are already applied are skipped.
set -uo pipefail
cd "$(dirname "$0")/.."
status=0
for tree in third_party/same_cdi third_party/same_cdi_ps5; do
    [ -d "$tree/.git" ] || continue
    for p in patches/*.patch; do
        name=$(basename "$p")
        if git -C "$tree" apply --check "$PWD/$p" 2>/dev/null; then
            git -C "$tree" apply "$PWD/$p" && echo "applied $name to $tree"
        elif git -C "$tree" apply --reverse --check "$PWD/$p" 2>/dev/null; then
            echo "$name is already applied to $tree"
        else
            echo "WARNING: $name does not apply cleanly to $tree" >&2
            status=1
        fi
    done
done
exit $status
