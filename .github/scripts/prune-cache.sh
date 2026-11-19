#!/usr/bin/env bash
# Delete every cache entry whose key starts with the given prefix.
# Usage: prune-cache.sh <owner/repo> <key-prefix>   (needs GH_TOKEN)
set -euo pipefail
repo="$1"
prefix="$2"
gh api "repos/$repo/actions/caches?per_page=100" --paginate \
  --jq ".actions_caches[] | select(.key | startswith(\"$prefix\")) | .id" |
while read -r id; do
  [ -n "$id" ] || continue
  gh api -X DELETE "repos/$repo/actions/caches/$id" >/dev/null || true
done
