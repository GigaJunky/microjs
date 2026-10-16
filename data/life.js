// Conway's Game of Life. Tap (or Space) to re-seed, p pauses. Only cells that change are redrawn.
const CELL = lcd.height() < 200 ? 8 : 16;  // 20 x 15 cells on the CYD, 30 x 16 on the Cardputer
const COLS = Math.floor(lcd.width() / CELL), ROWS = Math.floor(lcd.height() / CELL);
let grid = new Array(COLS * ROWS).fill(0);
let next = new Array(COLS * ROWS).fill(0);  // reused every generation (no big allocations)
let gen = 0;

function seed() {
  for (let i = 0; i < grid.length; i++) grid[i] = Math.random() < 0.3 ? 1 : 0;
  gen = 0;
  lcd.fillScreen(lcd.BLACK);
  grid.forEach((v, i) => v && drawCell(i, 1));
}

function drawCell(i, alive) {
  const x = (i % COLS) * CELL, y = Math.floor(i / COLS) * CELL;
  if (alive) lcd.fillRoundRect(x + 1, y + 1, CELL - 2, CELL - 2, 2, lcd.GREENYELLOW);
  else lcd.fillRect(x, y, CELL, CELL, lcd.BLACK);
}

function step() {
  for (let r = 0; r < ROWS; r++) {
    const up = ((r + ROWS - 1) % ROWS) * COLS, mid = r * COLS, down = ((r + 1) % ROWS) * COLS;
    for (let c = 0; c < COLS; c++) {
      const l = (c + COLS - 1) % COLS, rt = (c + 1) % COLS;
      const n = grid[up + l] + grid[up + c] + grid[up + rt] + grid[mid + l] + grid[mid + rt] +
                grid[down + l] + grid[down + c] + grid[down + rt];
      const i = mid + c;
      const alive = n === 3 || (n === 2 && grid[i]) ? 1 : 0;
      next[i] = alive;
      if (alive !== grid[i]) drawCell(i, alive);
    }
  }
  [grid, next] = [next, grid];
  gen++;
}

let paused = false;
touch.on("down", () => seed());
keyboard.on("key", ({ key }) => {
  if (key === " " || key === "Enter") seed();
  else if (key === "p") paused = !paused;
});
joystick.on("press", ({ button }) => (button === 2 ? (paused = !paused) : null));  // gamepad button 3 pauses
seed();
setInterval(() => (paused ? null : step()), 50);
