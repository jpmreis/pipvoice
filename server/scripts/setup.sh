#!/usr/bin/env bash
# Pip VPS setup (Ubuntu 24.04). Run as root. Usage:
#   ./setup.sh voice.example.com admin@example.com
set -euo pipefail
DOMAIN="${1:?domain}"; EMAIL="${2:?letsencrypt email}"
APP_DIR=/opt/pipvoice

# ---- packages ----
apt-get update -q
apt-get install -y python3-venv mosquitto mosquitto-clients certbot sqlite3 ffmpeg \
                   debian-keyring debian-archive-keyring apt-transport-https curl
# Caddy official repo
curl -1sLf https://dl.cloudsmith.io/public/caddy/stable/gpg.key \
  | gpg --dearmor --yes --batch -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt \
  > /etc/apt/sources.list.d/caddy-stable.list
apt-get update -q && apt-get install -y caddy

# ---- app user + tree ----
id -u pipvoice &>/dev/null || useradd -r -m -d $APP_DIR -s /usr/sbin/nologin pipvoice
mkdir -p $APP_DIR/data/audio $APP_DIR/data/firmware
cp -r "$(dirname "$0")/../app" $APP_DIR/
mkdir -p $APP_DIR/scripts
cp "$(dirname "$0")/backup.sh" $APP_DIR/scripts/
chmod +x $APP_DIR/scripts/backup.sh
cp "$(dirname "$0")/../requirements.txt" $APP_DIR/
python3 -m venv $APP_DIR/venv
$APP_DIR/venv/bin/pip install -q -r $APP_DIR/requirements.txt

# ---- secrets / env ----
MQTT_SERVER_PASS=$(openssl rand -base64 18)
cat > $APP_DIR/env <<ENV
PIP_DATA=$APP_DIR/data
PIP_BASE_URL=https://$DOMAIN
PIP_MQTT_HOST=127.0.0.1
PIP_MQTT_USER=server
PIP_MQTT_PASS=$MQTT_SERVER_PASS
ENV
chmod 600 $APP_DIR/env
chown -R pipvoice:pipvoice $APP_DIR

# ---- TLS certificates (webroot via Caddy) ----
# The real Caddyfile references the Let's Encrypt cert files, which don't
# exist yet: bootstrap with an HTTP-only config, obtain the cert, then
# switch to the full config.
mkdir -p /var/www/acme
cat > /etc/caddy/Caddyfile <<BOOT
{
    auto_https off
}
http://$DOMAIN {
    root * /var/www/acme
    file_server
}
BOOT
systemctl enable --now caddy
systemctl restart caddy
certbot certonly --webroot -w /var/www/acme -d "$DOMAIN" -m "$EMAIL" \
        --agree-tos --non-interactive
# renewal hook: broker + proxy pick up new certs
cat > /etc/letsencrypt/renewal-hooks/deploy/reload.sh <<'HOOK'
#!/bin/sh
# caddy and mosquitto run as unprivileged users that can't read
# /etc/letsencrypt: hand each a copy it owns.
cp /etc/letsencrypt/live/*/fullchain.pem /etc/mosquitto/certs/server.crt
cp /etc/letsencrypt/live/*/privkey.pem   /etc/mosquitto/certs/server.key
chown mosquitto: /etc/mosquitto/certs/server.*
cp /etc/letsencrypt/live/*/fullchain.pem /etc/caddy/certs/server.crt
cp /etc/letsencrypt/live/*/privkey.pem   /etc/caddy/certs/server.key
chown caddy: /etc/caddy/certs/server.*
chmod 600 /etc/mosquitto/certs/server.key /etc/caddy/certs/server.key
systemctl reload-or-restart mosquitto caddy
HOOK
chmod +x /etc/letsencrypt/renewal-hooks/deploy/reload.sh
mkdir -p /etc/mosquitto/certs /etc/caddy/certs
/etc/letsencrypt/renewal-hooks/deploy/reload.sh
cp "$(dirname "$0")/../Caddyfile" /etc/caddy/Caddyfile
sed -i "s/DOMAIN/$DOMAIN/g" /etc/caddy/Caddyfile
systemctl restart caddy

# ---- mosquitto ----
cp "$(dirname "$0")/../mosquitto.conf" /etc/mosquitto/conf.d/pipvoice.conf
# credentials live in a dir owned by the app user: mosquitto_passwd (run as
# pipvoice when provisioning devices) rewrites via a temp file in the same
# dir. setgid keeps group=mosquitto on new files so the broker can read them.
mkdir -p /etc/mosquitto/pipvoice
touch /etc/mosquitto/pipvoice/passwd /etc/mosquitto/pipvoice/acl
mosquitto_passwd -b /etc/mosquitto/pipvoice/passwd server "$MQTT_SERVER_PASS"
# seed only once: on re-runs the acl already carries per-device entries
grep -q '^user server$' /etc/mosquitto/pipvoice/acl \
  || printf 'user server\ntopic write dev/+/notify\n' >> /etc/mosquitto/pipvoice/acl
chown -R pipvoice:mosquitto /etc/mosquitto/pipvoice
chmod 2750 /etc/mosquitto/pipvoice
chmod 640 /etc/mosquitto/pipvoice/passwd /etc/mosquitto/pipvoice/acl
echo 'pipvoice ALL=(root) NOPASSWD: /usr/bin/systemctl reload mosquitto' \
  > /etc/sudoers.d/pipvoice
systemctl enable --now mosquitto && systemctl restart mosquitto

# ---- app service ----
cp "$(dirname "$0")/../systemd/pipvoice.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now pipvoice

# ---- ssh hardening ----
# key-only access; the provisioning flow already installed a key for root
printf 'PasswordAuthentication no\nKbdInteractiveAuthentication no\n' \
  > /etc/ssh/sshd_config.d/50-pipvoice.conf
systemctl reload ssh 2>/dev/null || systemctl reload sshd

# ---- firewall ----
ufw allow 22/tcp; ufw allow 80/tcp; ufw allow 443/tcp; ufw allow 8883/tcp
ufw --force enable

echo "Done. Open https://$DOMAIN/admin to create the admin account."
