#!/usr/bin/env bash
# Build one stage of a CMake tree, retrying while the clang frontend crashes or
# a package repository throttles. Usage: build.sh <build-dir> <log-name> <target...>
set -uo pipefail
dir="$1"
name="$2"
shift 2
log="$RUNNER_TEMP/build-$name.log"
attempts="${BUILD_ATTEMPTS:-6}"
for ((attempt = 1; ; ++attempt)); do
  if cmake --build "$dir" --parallel "${BUILD_JOBS:?BUILD_JOBS is unset}" --target "$@" 2>&1 | tee "$log"; then
    exit 0
  fi
  status=$?
  if ((attempt >= attempts)); then
    echo "Stage $name still failing after $attempt attempts." >&2
    exit "$status"
  fi
  if grep -Fq 'PLEASE submit a bug report to https://github.com/llvm/llvm-project/issues/' "$log"; then
    echo "Clang crashed during $name; retrying (attempt $attempt)."
  elif grep -Fq 'Received status code 429 from server: Too Many Requests' "$log"; then
    echo "A package repository rate-limited during $name; waiting before retrying."
    sleep "${RATE_LIMIT_WAIT:-30}"
  else
    exit "$status"
  fi
done
