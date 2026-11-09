#!/bin/bash
# Prompts for Wi-Fi details and writes them into include/secrets.h (gitignored).
# The password is not echoed and never leaves this machine except onto the ESP32.
set -euo pipefail
cd "$(dirname "$0")"
FILE=include/secrets.h
[ -f "$FILE" ] || { echo "Missing $FILE"; exit 1; }

read -r -p "Wi-Fi network name: " SSID
read -r -s -p "Wi-Fi password (hidden): " PASS; echo

SSID="$SSID" PASS="$PASS" .venv/bin/python - "$FILE" <<'EOF'
import os, re, sys
path = sys.argv[1]
c = lambda s: '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
text = open(path).read()
for key, val in (("WIFI_SSID", os.environ["SSID"]), ("WIFI_PASSWORD", os.environ["PASS"])):
    text = re.sub(rf'#define {key} .*', lambda m: f'#define {key} {c(val)}', text)
open(path, 'w').write(text)
EOF
chmod 600 "$FILE"
echo "Saved to $FILE for network \"$SSID\"."
