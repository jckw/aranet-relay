// Runs on the Shelly Plug US Gen4 (Scripts tab in its web UI). Every minute it posts the heater's
// power draw and the plug's energy counter to the Worker. Outbound HTTPS only; no router setup needed.
//
// Paste in the real POWER_TOKEN before saving it on the plug. Don't commit the token.

let URL = "https://aranet.jackweatherilt.workers.dev/power";
let TOKEN = "PASTE_POWER_TOKEN_HERE";
let EVERY_MS = 60 * 1000;

function post() {
  let s = Shelly.getComponentStatus("switch", 0);
  if (!s || typeof s.apower !== "number" || !s.aenergy) {
    print("power: no meter reading yet");
    return;
  }
  Shelly.call(
    "HTTP.Request",
    {
      method: "POST",
      url: URL,
      timeout: 20,
      headers: { "Content-Type": "application/json", "Authorization": "Bearer " + TOKEN },
      body: JSON.stringify({ watts: s.apower, energy: s.aenergy.total, on: s.output }),
    },
    function (res, err, msg) {
      if (err !== 0) print("power: request failed:", err, msg);
      else if (res.code !== 200) print("power: server said", res.code, res.body);
    }
  );
}

Timer.set(EVERY_MS, true, post);
post();
