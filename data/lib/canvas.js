// Browser-style canvas: the firmware has it built in (canvas.native), so a script just does
//   load("/lib/canvas.js"); const ctx = canvas.getContext("2d");
// and draws with the browser's calls (fillRect, beginPath / arc / lineTo / fill / stroke, fillText, save /
// translate / rotate ...) and requestAnimationFrame. See "Canvas" in the README for what is supported.
if (!(typeof canvas !== "undefined" && canvas.native)) throw new Error("this firmware has no canvas: update the board's firmware");

// the browser's animation timer: at most 30 frames a second, the callback gets the time in ms (and a buffered canvas is shown)
function requestAnimationFrame(fn) { return setTimeout(() => { fn(millis()); canvas.flush(); }, 33); }
function cancelAnimationFrame(id) { clearTimeout(id); }
