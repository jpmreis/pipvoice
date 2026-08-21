#!/bin/sh
# Mosquitto entrypoint for the Docker stack:
# 1. seed the shared auth files + the app's own broker account,
# 2. wait for Caddy's Let's Encrypt cert and copy it where mosquitto
#    can read it,
# 3. run the broker, HUP-reloading whenever the app rewrites the auth
#    files (device provisioning) or Caddy renews the cert.
set -eu
AUTH=/mosquitto-auth
CERTS=/mosquitto-certs
mkdir -p "$CERTS"

touch "$AUTH/passwd" "$AUTH/acl"
if ! grep -q "^server:" "$AUTH/passwd"; then
    mosquitto_passwd -b "$AUTH/passwd" server "$MQTT_SERVER_PASS"
fi
if [ ! -s "$AUTH/acl" ]; then
    printf 'user server\ntopic write dev/+/notify\ntopic read presence/+\n' \
        > "$AUTH/acl"
fi
chown -R mosquitto:mosquitto "$AUTH" "$CERTS"

find_cert() {
    find /caddy-data -name "${DOMAIN}.crt" 2>/dev/null | head -1
}

sync_certs() {   # returns 0 when the cert changed
    src=$(find_cert)
    [ -n "$src" ] || return 1
    key="${src%.crt}.key"
    if ! cmp -s "$src" "$CERTS/server.crt" 2>/dev/null; then
        cp "$src" "$CERTS/server.crt"
        cp "$key" "$CERTS/server.key"
        chown mosquitto:mosquitto "$CERTS/server.crt" "$CERTS/server.key"
        chmod 640 "$CERTS/server.crt" "$CERTS/server.key"
        return 0
    fi
    return 1
}

echo "waiting for Caddy to obtain the ${DOMAIN} certificate..."
until sync_certs; do sleep 5; done
echo "certificate ready, starting mosquitto"

mosquitto -c /pip/mosquitto.conf &
PID=$!
trap 'kill $PID' TERM INT

stamp() { ls -l "$AUTH/passwd" "$AUTH/acl" 2>/dev/null; }
LAST=$(stamp)
while kill -0 $PID 2>/dev/null; do
    sleep 15
    NOW=$(stamp)
    if [ "$NOW" != "$LAST" ] || sync_certs; then
        LAST=$NOW
        echo "auth/cert change detected - reloading broker"
        kill -HUP $PID
    fi
done
wait $PID
