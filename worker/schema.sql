CREATE TABLE IF NOT EXISTS readings (
  ts INTEGER PRIMARY KEY,          -- unix seconds when the sensor took the reading
  temperature REAL NOT NULL,       -- °C
  humidity INTEGER NOT NULL,       -- %
  pressure REAL NOT NULL,          -- hPa
  co2 INTEGER NOT NULL,            -- ppm
  battery INTEGER,                 -- %, only from live broadcasts
  source TEXT NOT NULL             -- 'live' (ESP32) or 'import' (on-board log)
);
CREATE TABLE IF NOT EXISTS bridge (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  last_seen INTEGER NOT NULL,      -- unix seconds of the bridge's last post
  rssi INTEGER
);
CREATE TABLE IF NOT EXISTS power (
  ts INTEGER PRIMARY KEY,          -- unix seconds when the Worker received the sample
  watts REAL NOT NULL,             -- instantaneous draw, W
  energy REAL NOT NULL,            -- plug's cumulative counter, Wh (can reset on plug reboot)
  output INTEGER NOT NULL          -- relay on (1) or off (0)
);
