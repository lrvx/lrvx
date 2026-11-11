#!/usr/bin/env bash
# Tests for scripts/ci-apt-install.sh with fake sudo / apt-get / dpkg on PATH.
# The real `timeout` is used, so the hang case is a real kill.
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin"

cat > "$work/bin/sudo" <<'FAKE'
#!/usr/bin/env bash
exec "$@"
FAKE

cat > "$work/bin/env" <<'FAKE'
#!/usr/bin/env bash
# Records the variables sudo was asked to set, then runs the command.
while [[ "$1" == *=* ]]; do
  echo "env $1" >> "$FAKE_LOG"
  export "$1"
  shift
done
exec "$@"
FAKE

# apt-get: logs its arguments, then acts on the plan for this sub-command.
# A plan is one word per call: ok, fail, or hang.
cat > "$work/bin/apt-get" <<'FAKE'
#!/usr/bin/env bash
echo "apt-get $*" >> "$FAKE_LOG"
plan_file="$FAKE_DIR/plan.$1"
action="$(head -n 1 "$plan_file" 2>/dev/null || echo ok)"
[[ -f "$plan_file" ]] && sed -i.bak '1d' "$plan_file"
case "$action" in
  hang) sleep 60 ;;
  fail) exit 100 ;;
  *) exit 0 ;;
esac
FAKE

cat > "$work/bin/dpkg" <<'FAKE'
#!/usr/bin/env bash
echo "dpkg $*" >> "$FAKE_LOG"
FAKE
chmod +x "$work/bin/"*

export PATH="$work/bin:$PATH"
export FAKE_DIR="$work" FAKE_LOG="$work/log"
export CI_APT_TIMEOUT=1 CI_APT_UPDATE_TIMEOUT=1 CI_APT_RETRY_PAUSE=0

failed=0
check() {
  if [[ "$1" == "$2" ]]; then
    echo "ok   $3"
  else
    echo "FAIL $3"
    echo "  want: $2"
    echo "  got:  $1"
    failed=1
  fi
}

run() {
  : > "$FAKE_LOG"
  rm -f "$work"/plan.*
  local sub
  for sub in update install; do
    local var="PLAN_$sub"
    [[ -n "${!var:-}" ]] && printf '%s\n' ${!var} > "$work/plan.$sub"
  done
  "$here/ci-apt-install.sh" "$@" > "$work/out" 2>&1
  status=$?
}

PLAN_update="" PLAN_install="" run --no-install-recommends cmake ninja-build
check "$status" 0 "clean run exits 0"
check "$(grep -v '^env ' "$FAKE_LOG")" $'apt-get update\napt-get install -y --no-install-recommends cmake ninja-build' \
  "update then install, options and packages passed through"

start=$SECONDS
PLAN_update="hang ok" PLAN_install="" run cmake
elapsed=$((SECONDS - start))
check "$status" 0 "a hung update is killed and the retry succeeds"
check "$(grep -v '^env ' "$FAKE_LOG")" $'apt-get update\napt-get update\napt-get install -y cmake' \
  "the hang costs one attempt and the install follows the retry"
check "$((elapsed < 20))" 1 "the hang was bounded by the time limit (took ${elapsed}s, the fake sleeps 60)"
check "$(grep -c 'timed out after 1s (attempt 1 of 3)' "$work/out")" 1 "the timeout is reported as a warning"

PLAN_update="" PLAN_install="fail fail fail" run cmake
check "$status" 1 "three failed installs exit 1"
check "$(grep -c '^apt-get install' "$FAKE_LOG")" 3 "install was tried three times, not more"
check "$(grep -v '^env ' "$FAKE_LOG" | tr '\n' ' ')" \
  "apt-get update apt-get install -y cmake dpkg --configure -a apt-get update apt-get install -y cmake dpkg --configure -a apt-get update apt-get install -y cmake " \
  "between install attempts dpkg is settled and the index refreshed"
check "$(grep -c '^env DEBIAN_FRONTEND=noninteractive' "$FAKE_LOG")" 8 "every privileged command gets DEBIAN_FRONTEND"
check "$(grep -c '::error::apt-get install failed after 3 attempts' "$work/out")" 1 "the failure is an error annotation"

PLAN_update="fail fail fail" PLAN_install="" run cmake
check "$status" 1 "a failed update exits 1"
check "$(grep -c '^apt-get install' "$FAKE_LOG")" 0 "install is not attempted after update failed"

PLAN_update="" PLAN_install="fail ok" run cmake
check "$status" 0 "one failed install then success exits 0"

start=$SECONDS
PLAN_update="" PLAN_install="hang ok" run cmake
check "$status" 0 "a hung install is killed and the retry succeeds"
check "$((SECONDS - start < 20))" 1 "the hung install was bounded"

run
check "$status" 2 "no packages is a usage error"

exit "$failed"
