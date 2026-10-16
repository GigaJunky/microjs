// fetch.js - the data fetcher for the weather/stocks/news dashboard.
//
// The only app that uses the network. It pulls each feed and writes it to its own small file on
// the SD card (or flash); the display app (weather.js) renders from those files with no WiFi
// stack, no TLS and no big response in memory. That split keeps the long-running display lean.
//
// It fetches ONE feed per run, re-launching itself for the next (a "stage" counter file). Each
// stage runs in a fresh interpreter with an unfragmented heap - the only way the CYD (no PSRAM)
// has enough contiguous memory for a feed. Each stage writes just its own file (no read-and-
// merge of a growing cache, which doubled memory and failed on the CYD). WiFi stays connected
// across the re-launches; the last stage disconnects it (freeing ~40 KB) before the display.
// weather.js re-runs this app when its data goes stale.
//
// Settings (micro-js.json): wifi, location, utcOffsetHours, weatherRefreshMinutes,
//   stocks: { apiKey, symbols: ["AAPL", ...] }
//   news:   headlines source. Default: Hacker News front page (HTTPS, so needs a PSRAM board;
//           the CYD can't do TLS with WiFi up). "news": false skips it; "news": { "url": "..." }
//           uses another JSON feed whose items carry a "title" (or story_title / headline).
// @needs-more-memory (big for an original ESP32 without PSRAM: the CYD's launcher hides it at first)
const cfg = readConfig();
const WIFI = cfg.wifi ?? {};
const PLACE = cfg.location ?? { name: "Berlin", lat: 52.52, lon: 13.41 };
const ST = cfg.stocks ?? {};
const SYMBOLS = (ST.symbols ?? []).slice(0, 9).map((s) => s.toUpperCase());
const W = lcd.width(), H = lcd.height();

// one small file per feed, plus a tiny freshness marker and the stage counter
const DIR = board.hasSd ? "/sd/" : "/";
const F_META = DIR + "wxmeta.json", F_MAIN = DIR + "wxmain.json";
const F_STOCK = DIR + "wxstock.json", F_NEWS = DIR + "wxnews.json", F_STAGE = DIR + "wxstage";
const sibling = (name) => ["/" + name, "/sd/" + name].find((p) => fs.exists(p)) ?? "/" + name;
const SELF = sibling("fetch.js"), DISPLAY = sibling("weather.js");

// Feed order matters for memory. Weather is the heavy fetch (a ~15 KB response), so it goes
// LAST: its only hand-off is to the display, which runs with WiFi off (plenty of heap to load).
// The light feeds (news, stocks) run first, so no heavy-fetch -> launch transition remains to
// run the fragmented CYD heap out of the contiguous memory a script load needs.
//
// News uses HTTPS by default, which needs ~40 KB of contiguous heap for the TLS handshake - more
// than a board without PSRAM (e.g. the CYD) has free once WiFi is up. So skip news on low-memory
// boards unless the user pointed "news" at their own (plain-http) feed.
const newsOk = cfg.news !== false && (board.psram || (cfg.news && cfg.news.url));
const STAGES = [];
if (newsOk) STAGES.push("news");
if (ST.apiKey && SYMBOLS.length) STAGES.push("stocks");
STAGES.push("weather");

function msg(str, color) {
  lcd.fillScreen(lcd.BLACK);
  lcd.setTextDatum(lcd.MC);
  lcd.setTextColor(color ?? lcd.WHITE, lcd.BLACK);
  lcd.drawString(str, W / 2, H / 2, 2);
}

function getWeather() {
  const res = http.get(`http://api.open-meteo.com/v1/forecast?latitude=${PLACE.lat}&longitude=${PLACE.lon}` +
    "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m" +
    "&hourly=temperature_2m,weather_code,precipitation_probability" +
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset" +
    "&forecast_days=5&forecast_hours=12&timezone=auto");
  if (res.status !== 200) throw new Error(`HTTP ${res.status}`);
  const d = JSON.parse(res.body);
  const hm = (t) => t.slice(11, 16);  // "05:00" not "2026-10-14T05:00"
  d.current.time = hm(d.current.time);
  d.hourly.time = d.hourly.time.map(hm);
  d.daily.sunrise = d.daily.sunrise.map(hm);
  d.daily.sunset = d.daily.sunset.map(hm);
  d.daily.time = d.daily.time.map((t) => t.slice(0, 10).split("-").map(Number));
  return { place: PLACE.name, current: d.current, hourly: d.hourly, daily: d.daily, utcOffset: d.utc_offset_seconds };
}

function getStocks() {
  const res = http.get(`http://api.twelvedata.com/quote?symbol=${SYMBOLS.join(",")}&apikey=${ST.apiKey}`);
  const d = JSON.parse(res.body);
  if (d.status === "error") throw new Error(String(d.message).slice(0, 48));
  const list = SYMBOLS.length === 1 ? [d] : SYMBOLS.map((s) => d[s]);
  const stocks = list.map((q, i) => q && q.close
    ? { sym: SYMBOLS[i], price: +q.close, chg: +q.change, pct: +q.percent_change }
    : { sym: SYMBOLS[i], err: q && q.message ? String(q.message).slice(0, 20) : "no data" });
  const first = list.find((q) => q && q.close);
  return { stocks, marketOpen: first ? !!first.is_market_open : null };
}

function getNews() {
  const url = (cfg.news && cfg.news.url) || "https://hn.algolia.com/api/v1/search?tags=front_page&hitsPerPage=8";
  const res = http.get(url);
  if (res.status !== 200) throw new Error(`HTTP ${res.status}`);
  const d = JSON.parse(res.body);
  const arr = d.hits || d.articles || d.results || [];
  return arr.map((a) => a.title || a.story_title || a.headline).filter(Boolean).slice(0, 6);
}

function doStage(name) {
  if (name === "weather") {
    try { fs.write(F_MAIN, JSON.stringify(Object.assign(getWeather(), { weatherError: "" }))); }
    catch (e) { console.log("weather:", e.message); fs.write(F_MAIN, JSON.stringify({ weatherError: e.message })); }
  } else if (name === "stocks") {
    try { const s = getStocks(); fs.write(F_STOCK, JSON.stringify({ stocks: s.stocks, marketOpen: s.marketOpen })); }
    catch (e) { console.log("stocks:", e.message); fs.write(F_STOCK, JSON.stringify({ stocks: [], stocksError: e.message })); }
  } else if (name === "news") {
    try { fs.write(F_NEWS, JSON.stringify({ news: getNews() })); }
    catch (e) { console.log("news:", e.message); fs.write(F_NEWS, JSON.stringify({ news: [], newsError: e.message })); }
  }
}

function run() {
  if (wifi.status() !== "connected") {
    msg(`connecting to ${WIFI.ssid}...`);
    led(0, 0, 20);
    if (!wifi.connect(WIFI.ssid, WIFI.pass, 20000)) {
      led(0, 0, 0);
      msg("WiFi failed - retry in 30 s", lcd.RED);
      return setTimeout(run, 30000);
    }
    wifi.ntp("UTC0");  // board clock; it keeps running in the display app with WiFi off
  }

  const stage = Number(fs.read(F_STAGE)) || 0;
  const name = STAGES[stage];
  msg(`updating ${name}...`);
  led(0, 0, 20);
  doStage(name);
  led(0, 0, 0);

  if (stage + 1 < STAGES.length) {
    fs.write(F_STAGE, String(stage + 1));
    launch(SELF);  // next feed, fresh interpreter, clean heap
  } else {
    fs.write(F_META, JSON.stringify({ epoch: Math.floor(Date.now() / 1000) }));
    fs.remove(F_STAGE);
    // launching the display (which contains no "wifi.connect") makes the firmware free the WiFi
    // driver before loading it, so there is plenty of contiguous heap - no explicit disconnect.
    launch(DISPLAY);
  }
}

if (!WIFI.ssid) msg("Set wifi.ssid and wifi.pass in micro-js.json", lcd.ORANGE);
else run();
