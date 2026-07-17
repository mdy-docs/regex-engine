#!/bin/sh
# Fetches the tc39/test262 subset test/test262_runner.mjs runs (harness +
# RegExp built-ins + regexp literal grammar), sparse and shallow, pinned to
# a known commit so conformance results and test/test262.expectations stay
# reproducible -- bump TEST262_SHA deliberately, re-triage expectations, and
# commit both together. The checkout lands in test262/ (gitignored), same
# cached-external-data pattern as .ucd_cache/.
set -e

TEST262_SHA=f2d1435644797268dca1f7988cad5a4e89ccd8d2
DEST="$(dirname "$0")/../test262"

if [ -d "$DEST" ] && [ "$(git -C "$DEST" rev-parse HEAD 2>/dev/null)" = "$TEST262_SHA" ]; then
    echo "test262 already at $TEST262_SHA"
    exit 0
fi

rm -rf "$DEST"
git init -q "$DEST"
cd "$DEST"
git remote add origin https://github.com/tc39/test262.git
git sparse-checkout set harness test/built-ins/RegExp test/language/literals/regexp
# GitHub supports fetching an arbitrary commit by SHA at depth 1.
git fetch -q --depth 1 origin "$TEST262_SHA"
git checkout -q FETCH_HEAD
echo "test262 checked out at $TEST262_SHA"
