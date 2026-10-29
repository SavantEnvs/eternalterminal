#!/usr/bin/env bash
#
# mayhem/test.sh — RUN this repo's OWN functional test suite (already built by mayhem/build.sh).
# exit 0 = pass. EDIT per repo. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

# Run the project's own Catch2 suite (built by build.sh in testbuild/ with normal flags). The
# subset is the deterministic unit-level tags: crypto, framing, connection/forwarding state
# machines, parsers, session store, security regressions. Socket/pty/end-to-end tags are left out
# (multi-minute and timing-sensitive). Each Catch2 assertion compares an actual value to an
# expected one, so a patch that makes the program exit(0) or print nothing fails here.
RUNNER="$SRC/testbuild/et-test"
[ -x "$RUNNER" ] || { echo "missing $RUNNER: mayhem/build.sh did not build the test runner" >&2; exit 1; }

FILTER='[CryptoHandler],[ClientConnection],[ServerConnection],[PortForwardHandler],[ForwardSourceHandler],[ForwardDestinationHandler],[Osc133],[ControlMode],[MultiplexerState],[RawSocketUtils],[security],[FdPoller],[PollSet],[ClientArgParsing],[InjectionFilter],[PaneScreen],[SocksUtils],[TitleParser],[SessionStore],[WriteBuffer],[SecurityNotice],[BackedIO],[Connection]'
MIN_PASSED=200   # guards against the filter silently selecting (or the runner silently running) nothing

OUT="$(mktemp)"; trap 'rm -f "$OUT"' EXIT
timeout 900 "$RUNNER" "$FILTER" >"$OUT" 2>&1
rc=$?
tail -n 5 "$OUT"

# Catch2 summary: "test cases:  266 |  254 passed | 12 skipped" (failed field only when non-zero)
line="$(grep -E '^test cases: ' "$OUT" | tail -n 1)"
field() { sed -nE "s/.*[| ]+([0-9]+) $1.*/\1/p" <<<"$line" | head -n 1; }
passed="$(field passed)"; failed="$(field failed)"; skipped="$(field skipped)"
passed="${passed:-0}"; failed="${failed:-0}"; skipped="${skipped:-0}"
if [ -z "$line" ]; then   # Catch2 prints this instead when nothing failed or skipped
  all="$(sed -nE 's/^All tests passed \([0-9]+ assertions? in ([0-9]+) test cases?\).*/\1/p' "$OUT" | tail -n 1)"
  [ -z "$all" ] || { passed="$all"; line="all passed"; }
fi

if [ -z "$line" ] || [ "$rc" -ne 0 ] || [ "$passed" -lt "$MIN_PASSED" ]; then
  echo "et-test rc=$rc passed=$passed (need >= $MIN_PASSED) summary='$line'" >&2
  [ "$failed" -gt 0 ] || failed=1
fi
emit_ctrf "catch2" "$passed" "$failed" "$skipped"
