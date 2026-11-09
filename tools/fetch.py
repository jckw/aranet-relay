"""Download the Aranet4's full on-board history (about 17 days at 5-minute intervals) to history.csv.

Needs this Mac to be paired with the sensor once (run `aranetctl <address> -r` and enter the PIN
shown on the device). `aranetctl -r -o` returns no rows on firmware v2.0.12, hence this script.

    python tools/fetch.py [device-address] [out.csv]
Find the address with `aranetctl --scan`.
"""
import asyncio, csv, datetime as dt, sys
from aranet4.client import Aranet4, Param

ADDR = sys.argv[1] if len(sys.argv) > 1 else '4DAA7D21-8207-CDC2-15A4-28DA8E60D111'
OUT = sys.argv[2] if len(sys.argv) > 2 else 'history.csv'

async def main():
    a = Aranet4(address=ADDR); await a.connect()
    n = await a.get_total_readings(); interval = await a.get_interval(); ago = await a.get_seconds_since_update()
    now = dt.datetime.now().replace(microsecond=0)
    cols = {p.name.lower(): await a.get_records(p, log_size=n, start=1, end=n)
            for p in (Param.TEMPERATURE, Param.HUMIDITY, Param.PRESSURE, Param.CO2)}
    last = now - dt.timedelta(seconds=ago)
    with open(OUT, 'w', newline='') as f:
        w = csv.writer(f); w.writerow(['date', *cols])
        for i in range(n):
            t = last - dt.timedelta(seconds=interval * (n - 1 - i))
            w.writerow([t.isoformat(), *(cols[c][i] for c in cols)])
    print(f'{n} rows to {OUT}, {last - dt.timedelta(seconds=interval*(n-1))} -> {last}')

asyncio.run(main())
