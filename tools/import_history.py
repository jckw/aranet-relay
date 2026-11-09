"""Upload a history.csv from tools/fetch.py to the relay, filling gaps the live bridge missed.

    ARANET_URL=https://... INGEST_TOKEN=... python tools/import_history.py [history.csv]
Timestamps in the CSV are local time. Rows already stored are skipped server-side.
"""
import csv, datetime as dt, json, os, sys, urllib.request

path = sys.argv[1] if len(sys.argv) > 1 else 'history.csv'
rows = [{"ts": int(dt.datetime.fromisoformat(r['date']).timestamp()),
         "temperature": float(r['temperature']), "humidity": float(r['humidity']),
         "pressure": float(r['pressure']), "co2": float(r['co2'])}
        for r in csv.DictReader(open(path))]
req = urllib.request.Request(
    os.environ['ARANET_URL'].rstrip('/') + '/import', data=json.dumps({"rows": rows}).encode(),
    headers={"authorization": "Bearer " + os.environ['INGEST_TOKEN'], "content-type": "application/json",
             "user-agent": "aranet-import"})
print(urllib.request.urlopen(req).read().decode())
