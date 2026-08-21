#!/usr/bin/env bash
# Nightly backup: DB snapshot + audio. Add to root crontab:
#   15 3 * * * /opt/pip/scripts/backup.sh
set -euo pipefail
DEST=/opt/pipvoice/backups
mkdir -p "$DEST"
STAMP=$(date +%F)
sqlite3 /opt/pipvoice/data/pip.db ".backup '$DEST/db-$STAMP.sqlite'"
tar czf "$DEST/audio-$STAMP.tgz" -C /opt/pipvoice/data audio
# settings + secrets: env (opt/pipvoice/env on bare installs,
# root/pipvoice/.env on the Docker stack), VAPID key (regenerating it
# kills push subs), mosquitto per-device credentials.
# --ignore-failed-read skips whichever layout's files don't exist.
tar czf "$DEST/config-$STAMP.tgz" --ignore-failed-read -C / \
    opt/pipvoice/env root/pipvoice/.env \
    opt/pipvoice/data/vapid_private.pem etc/mosquitto/pipvoice
chmod 600 "$DEST"/config-*.tgz
ls -t "$DEST"/db-* | tail -n +15 | xargs -r rm         # keep 14 days
ls -t "$DEST"/audio-* | tail -n +15 | xargs -r rm
ls -t "$DEST"/config-* | tail -n +15 | xargs -r rm
