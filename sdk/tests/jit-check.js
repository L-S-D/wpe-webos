// SPDX-License-Identifier: GPL-3.0-or-later
// JavaScript results that must not depend on the tier (LLInt vs Baseline JIT):
// doubles, Math, typed arrays, sorting, number formatting. Run with JIT on and
// off and compare the first line. The second line is the time.
function f1(n) { let s = 0; for (let i = 1; i < n; i++) s += Math.sqrt(i) * 1.000001 + (i % 7) / 3 - Math.floor(i / 3.3) + Math.abs(Math.sin(i)) * 0.5; return s; }
function f2(n) { const a = new Float64Array(n); for (let i = 0; i < n; i++) a[i] = i * 0.37 + Math.pow(1.0001, i % 100); let s = 0; for (let i = 0; i < n; i++) s += a[i] / (1 + (i & 3)); return s; }
function f3(n) { let s = 0; for (let i = 0; i < n; i++) { const x = (i * 2654435761) >>> 0; s ^= (x >>> 3) + (x | 0); } return s; }
function f4(n) { const o = []; for (let i = 0; i < n; i++) o.push({ v: i * 1.5, s: String(i) }); o.sort((a, b) => b.v - a.v); return o[0].v + o[n - 1].v + o.length; }
function f5(n) { let s = ''; for (let i = 0; i < n; i++) s += (i * 1.1).toFixed(2).slice(-3); return s.length + parseFloat(s.slice(0, 10)); }
function f6(n) { let s = 0; for (let i = 0; i < n; i++) s += Math.exp((i % 50) / 50) + Math.log(i + 1) + Math.atan2(i, 7) + Math.cbrt(i) + Math.hypot(i, 3); return s; }
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
const t = Date.now();
const r = [f1(200000), f2(200000), f3(500000), f4(50000), f5(20000), f6(100000), fib(22),
           Math.round(1e9 * Math.cos(1.234)), (0.1 + 0.2).toString(), Number.parseFloat("3.14159e10"),
           Math.max(...[1.5, 2.5, -3]), (1e21).toString(), (123.456).toPrecision(5), Math.fround(5.5555),
           Math.trunc(-4.7), Math.round(-2.5), (-0).toString(), 1 / -0, (2 ** 53 + 1).toString()];
print(JSON.stringify(r));
print("ms " + (Date.now() - t));
