#!/usr/bin/env bash
# Rolling Hetzner snapshot of pip-prod: if the newest snapshot is older
# than INTERVAL_DAYS, create a fresh one, then delete the older ones —
# only after the new one succeeded, so there is always at least one good
# snapshot. Steady-state cost = a single snapshot.
#
# Runs on the operator's Mac (launchd: com.pip.snapshot), NOT on the
# server: the API token stays off the box, so a compromised server cannot
# delete its own recovery images. launchd triggers it daily + at load and
# the script self-throttles — macOS has no anacron, so a fixed weekly
# firing would be skipped entirely if the Mac were powered off at the time.
# Hetzner-only, BSD date (macOS). Needs the hcloud CLI with a configured
# context (~/.config/hcloud/cli.toml).
set -euo pipefail
HCLOUD="${HCLOUD:-/opt/homebrew/bin/hcloud}"   # launchd has a bare PATH
SERVER="${PIP_SNAPSHOT_SERVER:-pipvoice-prod}"
INTERVAL_DAYS=7

SNAPS=$($HCLOUD image list --type snapshot -o noheader -o columns=id,description \
        | awk -v s="$SERVER" 'index($0, s)')
NEWEST_DATE=$(echo "$SNAPS" | grep -oE '[0-9]{4}-[0-9]{2}-[0-9]{2}' | sort | tail -1)
if [ -n "$NEWEST_DATE" ]; then
    AGE_DAYS=$(( ($(date +%s) - $(date -j -f %Y-%m-%d "$NEWEST_DATE" +%s)) / 86400 ))
    if [ "$AGE_DAYS" -lt "$INTERVAL_DAYS" ]; then
        exit 0    # not due yet - quiet no-op
    fi
fi

OLD_IDS=$(echo "$SNAPS" | awk '{print $1}')
$HCLOUD server create-image --type snapshot \
        --description "$SERVER auto $(date +%F)" "$SERVER"
for id in $OLD_IDS; do
    $HCLOUD image delete "$id"
done
echo "$(date '+%F %T') snapshot rotated (deleted: ${OLD_IDS:-none})"
