#!/usr/bin/env bash
# Pins deps.lock, and library.json's dependencies with it, to the siblings'
# commits: each one's HEAD in ../<repo>, or the commit given as repo=sha
# (TesserAPI=390dd57). Refuses a commit GitHub doesn't have, since CI and
# PlatformIO fetch from there. With --check, changes nothing and fails when
# library.json and deps.lock disagree (release.yml runs it).
set -euo pipefail
cd "$(dirname "$0")/.."

lock=deps.lock
pins() { grep -E '^[^#[:space:]]+@[0-9a-f]{40}$' "$lock"; }

check() {
    local ok=0 repo sha url want
    while IFS=@ read -r repo sha; do
        want="https://github.com/$repo.git#$sha"
        url=$(jq -r --arg n "${repo#*/}" '.dependencies[] | select(.name == $n) | .version' library.json)
        if [ -n "$url" ] && [ "$url" != "$want" ]; then
            echo "library.json: ${repo#*/} is $url, deps.lock pins $sha" >&2
            ok=1
        fi
    done < <(pins)
    return $ok
}

if [ "${1:-}" = "--check" ]; then
    check && echo "library.json matches deps.lock"
    exit
fi

out=$(mktemp)
grep -E '^#' "$lock" > "$out"
while IFS=@ read -r repo old; do
    name=${repo#*/}
    ref=HEAD
    for arg in "$@"; do [ "${arg%%=*}" = "$name" ] && ref=${arg#*=}; done
    dir=../$name
    sha=$(git -C "$dir" rev-parse --verify "$ref^{commit}")
    if [ -n "$(git -C "$dir" status --porcelain --untracked-files=no)" ] && [ "$ref" = HEAD ]; then
        echo "note: $dir has uncommitted changes; pinning its HEAD, $sha" >&2
    fi
    if ! git ls-remote "https://github.com/$repo.git" | grep -q "^$sha"; then
        # Not a branch or tag head: is it in main's history?
        git -C "$dir" fetch -q origin main
        if ! git -C "$dir" merge-base --is-ancestor "$sha" origin/main; then
            echo "$repo $sha isn't pushed: push it first" >&2
            rm -f "$out"
            exit 1
        fi
    fi
    [ "$sha" != "$old" ] && echo "$repo ${old:0:7} -> ${sha:0:7}"
    echo "$repo@$sha" >> "$out"
done < <(pins)
mv "$out" "$lock"

# Only the dependencies' URLs change: the file keeps its layout.
while IFS=@ read -r repo sha; do
    url="https://github.com/$repo.git"
    perl -pi -e "s{\"\Q$url\E(#[0-9a-f]*)?\"}{\"$url#$sha\"}g" library.json
done < <(pins)
check
