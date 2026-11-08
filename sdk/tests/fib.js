// SPDX-License-Identifier: GPL-3.0-or-later
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
let t = Date.now(); fib(25); print("fib25_ms " + (Date.now() - t));
