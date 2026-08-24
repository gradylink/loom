#!/usr/bin/env bash
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOOM_EXEC="${LOOM_EXEC:-$ROOT/build/loom}"

TEST_DIR="$ROOT/tests"

if [ ! -d "$TEST_DIR" ]; then
  echo "No $TEST_DIR directory found; create tests/*.loom test files."
  exit 2
fi

if [ ! -x "$LOOM_EXEC" ]; then
  echo "Error: loom executable not found at $LOOM_EXEC"
  echo "Build the project (cmake --build build) or set LOOM_EXEC to the loom binary path."
  exit 2
fi

OUTDIR="$ROOT/tests/out"
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"

failures=0
shopt -s nullglob
for f in "$TEST_DIR"/*.loom; do
  name=$(basename "$f" .loom)
  echo "=== Running test: $name ==="
  od="$OUTDIR/$name"
  rm -rf "$od"
  mkdir -p "$od"

  echo "Compiling $f -> $od"
  "$LOOM_EXEC" "$f" -o "$od"
  rc=$?
  if [ $rc -ne 0 ]; then
    echo "FAIL: $name (compiler exited $rc)"
    failures=$((failures+1))
    continue
  fi

  if [ ! -f "$od/pack.mcmeta" ]; then
    echo "FAIL: $name missing pack.mcmeta"
    failures=$((failures+1))
    continue
  fi

  if ! find "$od/data" -type f -name '*.mcfunction' | grep -q .; then
    echo "FAIL: $name produced no mcfunction files"
    failures=$((failures+1))
    continue
  fi

  echo "PASS: $name"
done

if [ $failures -ne 0 ]; then
  echo "$failures tests failed"
  exit 1
fi

echo "All tests passed"
exit 0
