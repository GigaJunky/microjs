// Weather + stocks + news dashboard (display only). Pages rotate every 30 s; tap left/right or
// press , and / to change page. Reads the small files fetch.js cached (no WiFi here - that keeps
// it light), and re-launches fetch.js when the data is stale. Settings: weatherRefreshMinutes.
const cfg = readConfig();
const W = lcd.width(), H = lcd.height(), BG = lcd.NAVY, SMALL = H < 200;
const ROTATE_MS = 30000;
const DAYS = "SunMonTueWedThuFriSat", MONTHS = "JanFebMarAprMayJunJulAugSepOctNovDec";
const COND = "|0Clear|1Mostly clear|2Partly cloudy|3Overcast|45Fog|48Fog|51Drizzle|53Drizzle" +
  "|55Drizzle|56Frz drizzle|57Frz drizzle|61Light rain|63Rain|65Heavy rain|66Frz rain|67Frz rain" +
  "|71Light snow|73Snow|75Heavy snow|77Snow grains|80Showers|81Showers|82Heavy showers" +
  "|85Snow showers|86Snow showers|95Thunderstorm|96T-storm, hail|99T-storm, hail|";

// fetch.js writes one small file per feed; read each on its own (missing = that feed is absent)
const DIR = board.hasSd ? "/sd/" : "/";
const sibling = (name) => ["/" + name, "/sd/" + name].find((p) => fs.exists(p)) ?? "/" + name;
const FETCH = sibling("fetch.js");
function load(name) { try { const r = fs.read(DIR + name); return r ? JSON.parse(r) : null; } catch (e) { return null; } }

const main = load("wxmain.json");       // weather + place + utcOffset + weatherError
const stk = load("wxstock.json");       // stocks + marketOpen
const nws = load("wxnews.json");        // news
const meta = load("wxmeta.json");       // { epoch } freshness marker
const data = main;                      // weather errors live here (for the weather pages)

const wx = main && main.current ? { current: main.current, hourly: main.hourly, daily: main.daily } : null;
const PLACE_NAME = (main && main.place) || "Weather";
const STOCKS = (stk && stk.stocks) || [];
const MARKET = stk ? stk.marketOpen : null;
const NEWS = (nws && nws.news) || [];
const utcOffset = main ? (main.utcOffset ?? 0) : 0;
const hasStocks = STOCKS.length > 0;
const hasNews = NEWS.length > 0;
const HOURLY_TITLE = SMALL ? "Next 6 hours" : "Next 12 hours";

let PAGES = [];  // built once the draw functions below exist
const pageCount = () => PAGES.length;
let page = 0, lastFlip = millis();

const pad = (n) => String(n).padStart(2, "0");
const fahr = (c) => Math.round((c * 9) / 5 + 32);
const cf = (c) => `${Math.round(c)}C / ${fahr(c)}F`;
const month = (m) => MONTHS.slice(m * 3 - 3, m * 3);
function cond(code) {
  const i = COND.indexOf(`|${code}`);
  return i < 0 ? `code ${code}` : COND.slice(i + 1 + String(code).length, COND.indexOf("|", i + 1));
}

// date helpers (no Date object on the board): days since 1970-01-01 <-> y/m/d
function civil(days) {
  const z = days + 719468, era = Math.floor(z / 146097), doe = z - era * 146097;
  const yoe = Math.floor((doe - Math.floor(doe / 1460) + Math.floor(doe / 36524) - Math.floor(doe / 146096)) / 365);
  const doy = doe - (365 * yoe + Math.floor(yoe / 4) - Math.floor(yoe / 100));
  const mp = Math.floor((5 * doy + 2) / 153), m = mp < 10 ? mp + 3 : mp - 9;
  return { y: yoe + era * 400 + (m <= 2 ? 1 : 0), m, d: doy - Math.floor((153 * mp + 2) / 5) + 1 };
}
function daysFrom(y, m, d) {
  y -= m <= 2 ? 1 : 0;
  const era = Math.floor(y / 400), yoe = y - era * 400;
  const doy = Math.floor((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5) + d - 1;
  return era * 146097 + yoe * 365 + Math.floor(yoe / 4) - Math.floor(yoe / 100) + doy - 719468;
}
const weekday = (days) => { const i = (((days + 4) % 7) + 7) % 7; return DAYS.slice(i * 3, i * 3 + 3); };

function clock() {  // local date/time from the board clock (set over NTP by fetch.js) + offset
  const ms = Date.now();
  if (ms < 1.6e12) return "";
  const off = cfg.utcOffsetHours != null ? cfg.utcOffsetHours * 3600 : utcOffset;
  const t = Math.floor(ms / 1000) + off, days = Math.floor(t / 86400), s = t - days * 86400;
  const c = civil(days);
  return `${weekday(days)} ${c.d} ${month(c.m)}  ${pad(Math.floor(s / 3600))}:${pad(Math.floor(s / 60) % 60)}`;
}

function text(str, x, y, font, color, datum) {
  lcd.setTextDatum(datum ?? lcd.TL);
  lcd.setTextColor(color ?? lcd.WHITE);
  lcd.drawString(String(str), x, y, font);
}

function frame() {
  lcd.fillScreen(BG);
  const top = SMALL ? 18 : 22;
  lcd.fillRect(0, 0, W, top, lcd.BLACK);
  text(PAGES[page].title(), SMALL ? 3 : 6, top / 2 - 8, 2, lcd.SKYBLUE);
  text(SMALL ? clock().replace("  ", " ") : clock(), W - (SMALL ? 3 : 6), top / 2 - 8, 2, lcd.WHITE, lcd.TR);
  const n = pageCount();
  for (let i = 0; i < n; i++) lcd.fillCircle(W / 2 + (i - (n - 1) / 2) * 14, H - (SMALL ? 4 : 7), SMALL ? 2 : 3, i === page ? lcd.WHITE : lcd.DARKGREY);
}
const stocksTitle = () => "Stocks" + (MARKET === null ? "" : MARKET ? "  market open" : "  market closed");

function drawNow() {
  const c = wx.current, t = wx.daily;
  if (SMALL) {
    text(Math.round(c.temperature_2m), 6, 24, 7, lcd.WHITE);
    const x = 6 + lcd.textWidth(String(Math.round(c.temperature_2m)), 7) + 4;
    text("C", x, 24, 4, lcd.WHITE);
    text(`${fahr(c.temperature_2m)}F`, x, 48, 4, lcd.LIGHTGREY);
    text(cond(c.weather_code), 128, 24, 2, lcd.YELLOW);
    text(`humidity ${c.relative_humidity_2m}%`, 128, 42, 2, lcd.LIGHTGREY);
    text(`wind ${c.wind_speed_10m} km/h`, 128, 58, 2, lcd.LIGHTGREY);
    text(`high ${cf(t.temperature_2m_max[0])}  low ${cf(t.temperature_2m_min[0])}`, 6, 80, 2, lcd.WHITE);
    text(`rain ${t.precipitation_probability_max[0]}%`, 6, 98, 2, lcd.SKYBLUE);
    text(`sun ${t.sunrise[0]}-${t.sunset[0]}`, W - 6, 98, 2, lcd.ORANGE, lcd.TR);
    text(`updated ${c.time}`, 6, 118, 1, lcd.DARKGREY);
    return;
  }
  text(Math.round(c.temperature_2m), 20, 34, 7, lcd.WHITE);
  const x = 20 + lcd.textWidth(String(Math.round(c.temperature_2m)), 7) + 6;
  text("C", x, 36, 4, lcd.WHITE);
  text(`${fahr(c.temperature_2m)}F`, x, 62, 4, lcd.LIGHTGREY);
  text(cond(c.weather_code), 190, 40, 4, lcd.YELLOW);
  text(`humidity ${c.relative_humidity_2m}%`, 190, 70, 2, lcd.LIGHTGREY);
  text(`wind ${c.wind_speed_10m} km/h`, 190, 88, 2, lcd.LIGHTGREY);
  text(`Today  high ${cf(t.temperature_2m_max[0])}   low ${cf(t.temperature_2m_min[0])}`, 20, 120, 2, lcd.WHITE);
  text(`rain chance ${t.precipitation_probability_max[0]}%`, 20, 142, 2, lcd.SKYBLUE);
  text(`sunrise ${t.sunrise[0]}   sunset ${t.sunset[0]}`, 20, 164, 2, lcd.ORANGE);
  text(`updated ${c.time}`, 20, 196, 1, lcd.DARKGREY);
}

function drawHourly() {
  const h = wx.hourly;
  for (let i = 0; i < Math.min(SMALL ? 6 : 12, h.time.length); i++) {
    const y = (SMALL ? 21 : 26) + i * 16, rain = h.precipitation_probability[i];
    const c = h.temperature_2m[i];
    text(h.time[i], SMALL ? 4 : 8, y, 2, lcd.SKYBLUE);
    text(cond(h.weather_code[i]), SMALL ? 46 : 60, y, 2, lcd.WHITE);
    if (SMALL) text(`${Math.round(c)}C/${fahr(c)}F`, 198, y, 2, lcd.YELLOW, lcd.TR);
    else text(cf(c), 190, y, 2, lcd.YELLOW);
    text(`${rain}%`, W - (SMALL ? 4 : 8), y, 2, rain >= 50 ? lcd.CYAN : lcd.LIGHTGREY, lcd.TR);
  }
}

function drawDaily() {
  const d = wx.daily;
  for (let i = 0; i < d.time.length; i++) {
    const y = 30 + i * 38, [yy, mm, dd] = d.time[i];
    const days = daysFrom(yy, mm, dd);
    if (SMALL) {  // one 21 px row per day, two lines of the small font
      const ys = 20 + i * 21, hi = d.temperature_2m_max[i], lo = d.temperature_2m_min[i];
      text(i === 0 ? "Today" : weekday(days), 4, ys + 3, 2, lcd.SKYBLUE);
      text(cond(d.weather_code[i]), 50, ys + 1, 1, lcd.WHITE);
      text(`rain ${d.precipitation_probability_max[i]}%`, 50, ys + 11, 1, lcd.LIGHTGREY);
      text(`${Math.round(hi)}C / ${Math.round(lo)}C`, W - 4, ys + 1, 1, lcd.ORANGE, lcd.TR);
      text(`${fahr(hi)}F / ${fahr(lo)}F`, W - 4, ys + 11, 1, lcd.LIGHTGREY, lcd.TR);
      continue;
    }
    text(i === 0 ? "Today" : weekday(days), 8, y, 2, lcd.SKYBLUE);
    text(`${dd} ${month(mm)}`, 8, y + 16, 1, lcd.LIGHTGREY);
    text(cond(d.weather_code[i]), 70, y, 2, lcd.WHITE);
    text(`rain ${d.precipitation_probability_max[i]}%`, 70, y + 16, 1, lcd.LIGHTGREY);
    text(`${cf(d.temperature_2m_max[i])}`, W - 8, y, 2, lcd.ORANGE, lcd.TR);
    text(`low ${cf(d.temperature_2m_min[i])}`, W - 8, y + 16, 1, lcd.LIGHTGREY, lcd.TR);
  }
}

function drawStocks() {
  const ROW = SMALL ? 17 : 23, TOP = SMALL ? 20 : 26;
  STOCKS.slice(0, SMALL ? 6 : 9).forEach((q, i) => {
    const y = TOP + i * ROW;
    text(q.sym, SMALL ? 4 : 8, y, 2, lcd.WHITE);
    if (q.err) return text(q.err, SMALL ? 60 : 90, y, 2, lcd.RED);
    const up = q.chg >= 0, color = up ? lcd.GREEN : lcd.RED;
    if (SMALL) {
      text(q.price.toFixed(2), 130, y, 2, lcd.WHITE, lcd.TR);
      return text(`${up ? "+" : ""}${q.pct.toFixed(2)}%`, W - 4, y, 2, color, lcd.TR);
    }
    text(q.price.toFixed(2), 150, y, 2, lcd.WHITE, lcd.TR);
    text(`${up ? "+" : ""}${q.chg.toFixed(2)}`, 230, y, 2, color, lcd.TR);
    text(`${up ? "+" : ""}${q.pct.toFixed(2)}%`, W - 8, y, 2, color, lcd.TR);
  });
}

// wrap a long string into up to maxLines lines of about maxChars each (word boundaries)
function wrap(s, maxChars, maxLines) {
  const words = String(s).split(" "), lines = [];
  let line = "";
  for (const w of words) {
    if (line && (line + " " + w).length > maxChars) {
      lines.push(line);
      if (lines.length >= maxLines) return lines;
      line = w;
    } else line = line ? line + " " + w : w;
  }
  if (line && lines.length < maxLines) lines.push(line);
  return lines;
}

function drawNews() {
  const lh = SMALL ? 12 : 18, font = SMALL ? 1 : 2, maxc = SMALL ? 38 : 44;
  let y = SMALL ? 22 : 30;
  for (const title of NEWS) {
    if (y > H - lh - 4) break;
    const lines = wrap(title, maxc, SMALL ? 1 : 2);
    text("- " + lines[0], 4, y, font, lcd.WHITE);
    y += lh;
    for (let k = 1; k < lines.length && y <= H - lh - 4; k++) { text("  " + lines[k], 4, y, font, lcd.LIGHTGREY); y += lh; }
  }
  if (nws && nws.newsError) text("(news update failed)", 4, H - (SMALL ? 10 : 14), 1, lcd.DARKGREY);
}

PAGES = [
  { title: () => PLACE_NAME, draw: drawNow, weather: true },
  { title: () => HOURLY_TITLE, draw: drawHourly, weather: true },
  { title: () => "Next 5 days", draw: drawDaily, weather: true },
];
if (hasStocks) PAGES.push({ title: stocksTitle, draw: drawStocks });
if (hasNews) PAGES.push({ title: () => "Top news", draw: drawNews });

function draw() {
  frame();
  const p = PAGES[page];
  if (p.weather && !wx) {
    const e = data && data.weatherError ? `weather failed: ${data.weatherError}` : "no data - fetching...";
    return text(e, SMALL ? 6 : 20, H / 2 - 8, 2, lcd.ORANGE);
  }
  p.draw();
  if (p.weather && data && data.weatherError) text("(last update failed)", SMALL ? 6 : 20, H - (SMALL ? 20 : 14), 1, lcd.DARKGREY);
}

function message(str, color) {
  lcd.fillScreen(lcd.BLACK);
  text(str, W / 2, H / 2, 2, color ?? lcd.WHITE, lcd.MC);
}

function flip(dir) {
  page = (page + dir + pageCount()) % pageCount();
  lastFlip = millis();
  draw();
}

function stale() {
  if (!meta || !meta.epoch) return true;
  const now = Date.now();
  if (now < 1.6e12) return false;  // no clock yet (fresh boot): wait, don't thrash the API
  return now / 1000 - meta.epoch >= (cfg.weatherRefreshMinutes ?? 10) * 60;
}
function fetchNow() {
  message("updating...", lcd.SKYBLUE);
  launch(FETCH);
}

touch.on("down", ({ x }) => {
  tone(1400, 10);
  flip(x < W / 2 ? -1 : 1);
});
keyboard.on("key", ({ key }) => {
  if (key === "," || key === "ArrowLeft") flip(-1);
  else if (key === "/" || key === "ArrowRight" || key === " ") flip(1);
  else if (key === "r" || key === "Enter") fetchNow();  // manual refresh (Cardputer)
});

if (!main && !meta) {
  // nothing cached yet - hand straight to the fetcher, which will launch us back with data
  message("fetching weather...");
  launch(FETCH);
} else {
  draw();
  if (stale()) setTimeout(fetchNow, 1500);  // show the cached data briefly, then refresh
  setInterval(() => (stale() ? fetchNow() : null), 60000);
  setInterval(() => (millis() - lastFlip >= ROTATE_MS ? flip(1) : null), 1000);
  setInterval(() => {  // keep the header clock current
    const top = SMALL ? 18 : 22;
    lcd.fillRect(W / 2, 0, W / 2, top, lcd.BLACK);
    text(SMALL ? clock().replace("  ", " ") : clock(), W - (SMALL ? 3 : 6), top / 2 - 8, 2, lcd.WHITE, lcd.TR);
  }, 20000);
}
