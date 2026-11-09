// Aranet4 relay: the ESP32 bridge posts readings here; JackApp reads them back.
//
//   POST /ingest   (INGEST_TOKEN)  one live broadcast from the bridge
//   POST /import   (INGEST_TOKEN)  bulk rows from the sensor's on-board log
//   GET  /readings (READ_TOKEN)    latest reading + series for the last N hours

interface Env {
  DB: D1Database;
  INGEST_TOKEN: string;
  READ_TOKEN: string;
}

type Reading = { ts: number; temperature: number; humidity: number; pressure: number; co2: number; battery?: number | null };

const json = (body: unknown, status = 200) =>
  new Response(JSON.stringify(body), { status, headers: { 'content-type': 'application/json', 'cache-control': 'no-store' } });

function authorized(req: Request, token: string): boolean {
  const got = new TextEncoder().encode(req.headers.get('authorization') ?? '');
  const want = new TextEncoder().encode(`Bearer ${token}`);
  return got.byteLength === want.byteLength && crypto.subtle.timingSafeEqual(got, want);
}

const isNum = (v: unknown, lo: number, hi: number): v is number => typeof v === 'number' && Number.isFinite(v) && v >= lo && v <= hi;

function valid(r: Partial<Reading>): r is Reading {
  return isNum(r.ts, 1.6e9, 4e9) && isNum(r.temperature, -40, 85) && isNum(r.humidity, 0, 100) && isNum(r.pressure, 300, 1200) && isNum(r.co2, 0, 10000);
}

async function ingest(req: Request, env: Env): Promise<Response> {
  const body = (await req.json().catch(() => null)) as Record<string, unknown> | null;
  if (!body) return json({ error: 'Body must be JSON.' }, 400);
  const now = Math.floor(Date.now() / 1000);
  const ago = isNum(body.ago, 0, 86400) ? body.ago : 0;
  const r: Partial<Reading> = {
    ts: now - ago,
    temperature: body.temperature as number,
    humidity: body.humidity as number,
    pressure: body.pressure as number,
    co2: body.co2 as number,
    battery: isNum(body.battery, 0, 100) ? body.battery : null,
  };
  if (!valid(r)) return json({ error: 'Reading is missing a field or out of range.' }, 400);
  const rssi = isNum(body.rssi, -127, 20) ? Math.round(body.rssi) : null;

  // The sensor re-broadcasts the same measurement every few seconds; keep one row per measurement.
  const interval = isNum(body.interval, 60, 3600) ? body.interval : 300;
  const window = Math.floor(interval / 2);
  const dup = await env.DB.prepare('SELECT ts FROM readings WHERE ts BETWEEN ?1 AND ?2 LIMIT 1').bind(r.ts - window, r.ts + window).first();
  const stmts = [env.DB.prepare('INSERT INTO bridge (id, last_seen, rssi) VALUES (1, ?1, ?2) ON CONFLICT(id) DO UPDATE SET last_seen = ?1, rssi = ?2').bind(now, rssi)];
  if (!dup) {
    stmts.push(env.DB.prepare("INSERT OR IGNORE INTO readings (ts, temperature, humidity, pressure, co2, battery, source) VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'live')")
      .bind(r.ts, r.temperature, Math.round(r.humidity), r.pressure, Math.round(r.co2), r.battery ?? null));
  }
  await env.DB.batch(stmts);
  return json({ stored: !dup, ts: r.ts });
}

async function importRows(req: Request, env: Env): Promise<Response> {
  const body = (await req.json().catch(() => null)) as { rows?: Partial<Reading>[] } | null;
  const rows = body?.rows;
  if (!Array.isArray(rows) || rows.length === 0 || rows.length > 10000) return json({ error: 'Send {rows: [...]} with 1 to 10,000 rows.' }, 400);
  const bad = rows.findIndex((r) => !valid(r));
  if (bad >= 0) return json({ error: `Row ${bad} is missing a field or out of range.` }, 400);
  const stmt = env.DB.prepare("INSERT OR IGNORE INTO readings (ts, temperature, humidity, pressure, co2, battery, source) VALUES (?1, ?2, ?3, ?4, ?5, NULL, 'import')");
  let inserted = 0;
  for (let i = 0; i < rows.length; i += 500) {
    const res = await env.DB.batch((rows.slice(i, i + 500) as Reading[]).map((r) => stmt.bind(r.ts, r.temperature, Math.round(r.humidity), r.pressure, Math.round(r.co2))));
    inserted += res.reduce((n, x) => n + (x.meta.changes ?? 0), 0);
  }
  return json({ received: rows.length, inserted });
}

async function readings(url: URL, env: Env): Promise<Response> {
  const hours = Math.min(Math.max(Number(url.searchParams.get('hours')) || 24, 1), 24 * 31);
  const since = Math.floor(Date.now() / 1000) - hours * 3600;
  const [series, latest, battery, bridge] = await Promise.all([
    env.DB.prepare('SELECT ts, temperature, humidity, pressure, co2 FROM readings WHERE ts >= ?1 ORDER BY ts').bind(since).all(),
    env.DB.prepare('SELECT ts, temperature, humidity, pressure, co2 FROM readings ORDER BY ts DESC LIMIT 1').first(),
    env.DB.prepare('SELECT battery FROM readings WHERE battery IS NOT NULL ORDER BY ts DESC LIMIT 1').first<{ battery: number }>(),
    env.DB.prepare('SELECT last_seen, rssi FROM bridge WHERE id = 1').first<{ last_seen: number; rssi: number | null }>(),
  ]);
  return json({
    latest,
    battery: battery?.battery ?? null,
    bridge: bridge ? { lastSeen: bridge.last_seen, rssi: bridge.rssi } : null,
    readings: series.results,
  });
}

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    const url = new URL(req.url);
    const route = `${req.method} ${url.pathname}`;
    if (route === 'POST /ingest' || route === 'POST /import') {
      if (!authorized(req, env.INGEST_TOKEN)) return json({ error: 'Unauthorized' }, 401);
      return route === 'POST /ingest' ? ingest(req, env) : importRows(req, env);
    }
    if (route === 'GET /readings') {
      if (!authorized(req, env.READ_TOKEN)) return json({ error: 'Unauthorized' }, 401);
      return readings(url, env);
    }
    return json({ error: 'Not found' }, 404);
  },
} satisfies ExportedHandler<Env>;
