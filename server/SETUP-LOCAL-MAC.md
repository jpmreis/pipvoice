# Pip — local test server on a Mac

Run the full API + admin site on your Mac before touching the VPS.
No TLS, no Mosquitto required - perfect for developing against the API,
clicking through the admin UI, and running the test suite.

## 1. Run it

```bash
cd server
python3 -m venv venv && source venv/bin/activate
pip install -r requirements.txt
export PIP_DATA="$PWD/data"          # DB + audio live here
export PIP_BASE_URL="http://localhost:8080"
# (every other PIP_* setting and its default: see env.example)
uvicorn app.main:app --host 127.0.0.1 --port 8080 --reload
```

Open http://localhost:8080/admin - the first visit creates your admin
account. Add users, set permissions, provision a test device, upload a
firmware .bin: everything works exactly as it will in production.
`--reload` restarts on code edits.

## 2. Verify

```bash
source venv/bin/activate
pip install httpx
brew install ffmpeg     # transcode + theme rendering (also brings libopus)
python3 smoke_test.py   # ~80 end-to-end checks (uses its own /tmp/vmtest)
```

Exercise the API by hand with a token from a provisioned device:

```bash
curl -H "Authorization: Bearer <token>" http://localhost:8080/v1/contacts
```

## 3. Optional: MQTT notifies locally

Without a broker the server logs "notify skipped" and devices would poll -
fine for API testing. To test push notifies:

```bash
brew install mosquitto
mosquitto -v -p 1883        # dev mode, anonymous, foreground
# in the server shell before starting uvicorn:
export PIP_MQTT_HOST=127.0.0.1 PIP_MQTT_PORT=1883
# watch notifications arrive:
mosquitto_sub -t 'dev/+/notify' -v
```

Send a message via the API and the notify appears in mosquitto_sub.

## Limitations vs. the VPS

- Real devices expect **https://** and **mqtts://** and validate
  certificates, so they cannot point at this plain-HTTP server. Use the
  Mac setup for server/API/admin development; use the VPS (SETUP.md) for
  end-to-end tests with hardware.
- The provisioning page still generates NVS CSVs, but with the local
  PIP_BASE_URL baked in - re-provision on the VPS for real devices.
