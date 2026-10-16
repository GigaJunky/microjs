// Hello world: text, shapes, the RGB LED, the light sensor and the speaker.
const W = lcd.width(), SMALL = lcd.height() < 200;

lcd.fillScreen(lcd.BLACK);
lcd.setTextDatum(lcd.MC);
lcd.setTextColor(lcd.YELLOW);
lcd.drawString(`Hello, ${board.name.split(" ")[0]}!`, W / 2, SMALL ? 18 : 36, 4);
lcd.setTextColor(lcd.WHITE);
lcd.drawString(`JavaScript on an ESP32 @ ${sys.cpuMhz()} MHz`, W / 2, SMALL ? 44 : 70, 2);

const rainbow = [lcd.RED, lcd.ORANGE, lcd.YELLOW, lcd.GREEN, lcd.CYAN, lcd.BLUE, lcd.MAGENTA];
rainbow.forEach((c, i) => lcd.fillSmoothCircle((W / 8) * (i + 1), SMALL ? 74 : 120, SMALL ? 12 : 16, c, lcd.BLACK));

// hue (0-360) -> [r, g, b]
function hsv(h) {
  const f = (n) => {
    const k = (n + h / 60) % 6;
    return Math.round(255 * (1 - Math.max(0, Math.min(k, 4 - k, 1))));
  };
  return [f(5), f(3), f(1)];
}

lcd.setTextDatum(lcd.TL);
lcd.setTextColor(lcd.LIGHTGREY, lcd.BLACK);
lcd.setTextPadding(200);
let hue = 0;
setInterval(() => {
  hue = (hue + 4) % 360;
  const [r, g, b] = hsv(hue);
  led(r >> 3, g >> 3, b >> 3);  // dimmed, the LED is bright
  const y = SMALL ? 96 : 170;
  lcd.drawString(`uptime: ${(millis() / 1000).toFixed(1)} s`, 20, y, 2);
  if (board.hasLight) lcd.drawString(`light sensor: ${light()}`, 20, y + 20, 2);
}, 50);

tone(880, 80);
console.log("hello.js running - press Ctrl-C to stop");
