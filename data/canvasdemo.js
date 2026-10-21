// Canvas demo: drawn with the browser's canvas calls (lib/canvas.js) instead of lcd, so the same
// drawing code would run in a web page. A rotating star, a bouncing ball with a trail, an analog clock
// face and a bezier wave, redrawn about 30 times a second. Tap or press a key to change the colour.
load("/lib/canvas.js");
const ctx = canvas.getContext("2d");
if (canvas.buffer) canvas.buffer(true);  // draw off-screen and show each frame at once (no flicker); a browser has no such call
const W = canvas.width, H = canvas.height, hues = ["#ff3366", "#ffaa00", "#44ddaa", "#66aaff", "#cc66ff"];
let hue = 0, t = 0, bx = 40, by = H * 0.7, vx = 3, vy = 2;
const trail = [];

function star(cx, cy, r, a) {
  ctx.beginPath();
  for (let i = 0; i < 10; i++) {
    const ang = a + (i * Math.PI) / 5, rr = i % 2 ? r * 0.45 : r;
    ctx[i ? "lineTo" : "moveTo"](cx + rr * Math.cos(ang), cy + rr * Math.sin(ang));
  }
  ctx.closePath();
}
function draw() {
  t += 0.04;
  ctx.fillStyle = "#101828";
  ctx.fillRect(0, 0, W, H);
  // the star, turning, in the chosen colour
  ctx.fillStyle = hues[hue];
  star(W * 0.25, H * 0.35, H * 0.22, t);
  ctx.fill();
  ctx.strokeStyle = "white";
  ctx.lineWidth = 1;
  star(W * 0.25, H * 0.35, H * 0.22, t);
  ctx.stroke();
  // a clock face with a second hand
  const cx = W * 0.75, cy = H * 0.35, r = H * 0.22;
  ctx.strokeStyle = "#88aacc";
  ctx.lineWidth = 3;
  ctx.beginPath();
  ctx.arc(cx, cy, r, 0, Math.PI * 2);
  ctx.stroke();
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(cx, cy);
  ctx.lineTo(cx + r * 0.85 * Math.cos(t * 2 - Math.PI / 2), cy + r * 0.85 * Math.sin(t * 2 - Math.PI / 2));
  ctx.stroke();
  // a ball bouncing with a fading trail
  bx += vx; by += vy;
  if (bx < 8 || bx > W - 8) vx = -vx;
  if (by < H * 0.6 || by > H - 20) vy = -vy;
  trail.push([bx, by]);
  if (trail.length > 8) trail.shift();
  trail.forEach(([x, y], i) => {
    ctx.fillStyle = `rgb(${40 + i * 20},${60 + i * 12},${120 + i * 16})`;
    ctx.beginPath();
    ctx.arc(x, y, 3 + i, 0, Math.PI * 2);
    ctx.fill();
  });
  // a wave along the bottom, and a caption
  ctx.strokeStyle = hues[(hue + 2) % hues.length];
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(0, H - 8);
  ctx.bezierCurveTo(W * 0.3, H - 8 - 14 * Math.sin(t), W * 0.6, H - 8 + 14 * Math.sin(t), W, H - 8);
  ctx.stroke();
  ctx.fillStyle = "white";
  ctx.font = "10px sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  ctx.fillText("canvas on lcd - tap to change colour", W / 2, 4);
  requestAnimationFrame(draw);
}
const next = () => (hue = (hue + 1) % hues.length);
touch.on("down", next);
keyboard.on("key", next);
requestAnimationFrame(draw);
