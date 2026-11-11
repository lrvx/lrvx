#!/usr/bin/env bash
# apt-get update + install for CI jobs, with a hard time limit per attempt
# and retries.
#
# A bare `apt-get update` has no outer bound. When a mirror stalls
# mid-download apt's own Acquire timeout (120 s by default) does not always
# fire, and the step then sits until the job's timeout-minutes kills it: a
# job that normally takes two minutes burned thirty and turned main red with
# no code involved. `timeout` bounds each attempt from outside whatever the
# cause, and a fresh attempt gets a fresh connection.
#
# Usage: scripts/ci-apt-install.sh [apt-get install options] package...
#   CI_APT_UPDATE_TIMEOUT  seconds per `apt-get update` attempt (default 90;
#                          a healthy one takes about ten)
#   CI_APT_TIMEOUT         seconds per `apt-get install` attempt (default 300)
#   CI_APT_ATTEMPTS        attempts per command (default 3)
#   CI_APT_RETRY_PAUSE     seconds between attempts (default 10)
set -uo pipefail

if [[ $# -eq 0 ]]; then
  echo "usage: $0 [apt-get install options] package..." >&2
  exit 2
fi

update_limit="${CI_APT_UPDATE_TIMEOUT:-90}"
install_limit="${CI_APT_TIMEOUT:-300}"
attempts="${CI_APT_ATTEMPTS:-3}"
pause="${CI_APT_RETRY_PAUSE:-10}"

# timeout runs under sudo so it can signal apt, which runs as root; it signals
# its whole process group, so apt's download helpers and dpkg go with it.
# DEBIAN_FRONTEND is set inside the sudo command: sudo may reset the caller's
# environment.
bounded() {
  local limit="$1"
  shift
  sudo env DEBIAN_FRONTEND=noninteractive timeout --kill-after=15 "$limit" "$@"
}

report() {
  local what="$1" status="$2" limit="$3" attempt="$4"
  if [[ $status -eq 124 || $status -eq 137 ]]; then
    echo "::warning::$what timed out after ${limit}s (attempt $attempt of $attempts)"
  else
    echo "::warning::$what exited $status (attempt $attempt of $attempts)"
  fi
}

for ((attempt = 1; attempt <= attempts; attempt++)); do
  bounded "$update_limit" apt-get update
  status=$?
  [[ $status -eq 0 ]] && break
  report "apt-get update" "$status" "$update_limit" "$attempt"
  if ((attempt == attempts)); then
    echo "::error::apt-get update failed after $attempts attempts"
    exit 1
  fi
  sleep "$pause"
done

for ((attempt = 1; attempt <= attempts; attempt++)); do
  bounded "$install_limit" apt-get install -y "$@"
  status=$?
  [[ $status -eq 0 ]] && exit 0
  report "apt-get install" "$status" "$install_limit" "$attempt"
  if ((attempt == attempts)); then
    echo "::error::apt-get install failed after $attempts attempts"
    exit 1
  fi
  # An install killed mid-flight leaves dpkg half-configured, and the next
  # apt-get refuses to run until that is finished. Bounded like the rest: a
  # hung maintainer script is one way to get here.
  bounded 120 dpkg --configure -a || true
  sleep "$pause"
  # A fetch that 404s means the index went stale under us; refresh it once
  # before trying again. Its failure is not fatal, the install decides.
  bounded "$update_limit" apt-get update || true
done
