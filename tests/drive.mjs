// SPDX-License-Identifier: GPL-3.0-only
// Drive a firmware in the FM-1 emulator (../emulateur) from a script: press buttons and keys, turn knobs,
// take screenshots, record the audio to a WAV. No browser needed.
//
//   node tests/drive.mjs build/tracker.fwsc SCRIPT OUTDIR
//
// SCRIPT: one command per line (or ';'-separated):
//   wait MS                 run the device for MS milliseconds of device time
//   press NAME [MS]         press and release a button (FX SCL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+)
//   hold NAME / release NAME
//   key N [MS]              press note key N (0 = F3 .. 26 = G5)
//   turn KNOB STEPS         SELECT PRESETS ALGORITHM K1 K2 K3 K4, + = clockwise
//   shot NAME               save OUTDIR/NAME.png
//   rec on / rec off        start / stop recording OUTDIR/audio.wav
//   leds                    print the lit LEDs (buttons by name, keys as kN)
//   pcs N                   sample the program counter N times (1 ms apart): where the firmware spends its time
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { deflateSync, crc32 } from "node:zlib";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const emu = resolve(here, "../../emulateur");
const { instantiateFm1 } = await import(resolve(emu, "web/fm1-host.js"));

const [fwPath, script, outDir = "build/drive"] = process.argv.slice(2);
mkdirSync(outDir, { recursive: true });
// EMU_WASM=path : another emulator core (e.g. the original one, as on the web) instead of ours
const x = await instantiateFm1(readFileSync(process.env.EMU_WASM || resolve(emu, "web/fm1.wasm")));
const mem = () => new Uint8Array(x.memory.buffer);
const msg = () => new TextDecoder().decode(mem().slice(x.fm1_message_ptr(), x.fm1_message_ptr() + x.fm1_message_len()));
const fw = readFileSync(fwPath);
const fp = x.fm1_alloc(fw.length);
mem().set(fw, fp);
if (x.fm1_load(fp, fw.length) !== 0) throw new Error(msg());
if (process.env.NOJIT) x.fm1_set_jit(0);

const BTN = { "OCT-": 0, "OCT+": 1, FX: 2, SCL: 3, ENV: 4, LFO: 5, EDIT: 6, GLO: 7, HOME: 8, SAVE: 9, ARP: 10,
              SEQ: 11, PLAY: 12, REC: 13 };
const ENC = { SELECT: 0, PRESETS: 1, ALGORITHM: 2, K1: 3, K2: 4, K3: 5, K4: 6 };
const CHUNK = 4096;
const aptr = x.fm1_alloc(CHUNK * 2 * 4);
let recording = false;
const audio = [];

function drainAudio() {
  for (;;) {
    const n = x.fm1_audio(aptr, CHUNK);
    if (!n) break;
    if (recording) audio.push(new Float32Array(x.memory.buffer, aptr, n * 2).slice());
    if (n < CHUNK) break;
  }
}

function run(ms) {
  const target = x.fm1_guest_seconds() + ms / 1000;
  while (x.fm1_guest_seconds() < target) {
    const st = x.fm1_run(2_000_000);
    drainAudio();
    if (st !== 0) { x.fm1_status(); throw new Error("emulator stopped: " + msg()); }
  }
}

let lastFrame = new Uint8Array(240 * 240 * 4);
function frame() {                           // fm1_lcd() is 0 when nothing changed since the last call
  const p = x.fm1_lcd();
  if (p) lastFrame = mem().slice(p, p + 240 * 240 * 4);
  return lastFrame;
}

function png(path) {
  const rgba = frame();
  const rows = [];
  for (let y = 0; y < 240; y++) { rows.push(Buffer.from([0])); rows.push(Buffer.from(rgba.subarray(y * 960, (y + 1) * 960))); }
  const chunk = (t, d) => {
    const l = Buffer.alloc(4); l.writeUInt32BE(d.length);
    const c = Buffer.alloc(4); c.writeUInt32BE(crc32(Buffer.concat([Buffer.from(t), d])) >>> 0);
    return Buffer.concat([l, Buffer.from(t), d, c]);
  };
  const h = Buffer.alloc(13); h.writeUInt32BE(240, 0); h.writeUInt32BE(240, 4); h[8] = 8; h[9] = 6;
  writeFileSync(path, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", h),
    chunk("IDAT", deflateSync(Buffer.concat(rows))), chunk("IEND", Buffer.alloc(0))]));
}

function wav(path) {
  const n = audio.reduce((a, b) => a + b.length, 0);
  const b = Buffer.alloc(44 + n * 2);
  b.write("RIFF", 0); b.writeUInt32LE(36 + n * 2, 4); b.write("WAVEfmt ", 8);
  b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20); b.writeUInt16LE(2, 22); b.writeUInt32LE(44100, 24);
  b.writeUInt32LE(44100 * 4, 28); b.writeUInt16LE(4, 32); b.writeUInt16LE(16, 34); b.write("data", 36);
  b.writeUInt32LE(n * 2, 40);
  let o = 44, peak = 0, sum = 0;
  for (const a of audio) for (const v of a) {
    const s = Math.max(-1, Math.min(1, v));
    peak = Math.max(peak, Math.abs(s)); sum += s * s;
    b.writeInt16LE(Math.round(s * 32767), o); o += 2;
  }
  writeFileSync(path, b);
  return { seconds: +(n / 2 / 44100).toFixed(2), peak: +peak.toFixed(3), rms: +Math.sqrt(sum / Math.max(1, n)).toFixed(4) };
}

// function start addresses from the build's disassembly (build/tracker.dis next to the .fwsc)
const syms = [];
try {
  let pending = null;
  for (const line of readFileSync(fwPath.replace(/[^/]+$/, "tracker.dis"), "utf8").split("\n")) {
    const lab = line.match(/^([A-Za-z_.][\w.]*):\s*$/);
    if (lab) { pending = lab[1]; continue; }
    const ins = line.match(/^\s*([0-9a-f]+):/);
    if (ins && pending) { syms.push([parseInt(ins[1], 16), pending]); pending = null; }
  }
  syms.sort((a, b) => a[0] - b[0]);
} catch { /* no disassembly: raw addresses */ }
const x_peek = (adr) => x.fm1_debug_peek(adr) >>> 0;
const where = (pc) => {
  let lo = 0, hi = syms.length - 1, best = null;
  while (lo <= hi) { const m = (lo + hi) >> 1; if (syms[m][0] <= pc) { best = syms[m]; lo = m + 1; } else hi = m - 1; }
  return best ? `${best[1]}+0x${(pc - best[0]).toString(16)}` : `0x${pc.toString(16)}`;
};

run(1500);                                   // boot and splash
for (const raw of script.split(/[;\n]/)) {
  const [cmd, a, b] = raw.trim().split(/\s+/);
  if (!cmd) continue;
  switch (cmd) {
    case "wait": run(Number(a)); break;
    case "press": x.fm1_key(BTN[a], 1); run(Number(b ?? 60)); x.fm1_key(BTN[a], 0); run(60); break;
    case "hold": x.fm1_key(BTN[a], 1); run(30); break;
    case "release": x.fm1_key(BTN[a], 0); run(30); break;
    case "holdkey": x.fm1_key(14 + Number(a), 1); run(30); break;
    case "releasekey": x.fm1_key(14 + Number(a), 0); run(30); break;
    case "key": x.fm1_key(14 + Number(a), 1); run(Number(b ?? 120)); x.fm1_key(14 + Number(a), 0); run(40); break;
    case "leds": { const p = x.fm1_leds(); const l = mem().slice(p, p + 41);
                   console.log("leds", "FX=" + l[2] + " HOME=" + l[8] + " EDIT=" + l[6], [...l].map((v, i) => v > 128 ? (i < 14 ? Object.keys(BTN)[i] : "k" + (i - 14)) : null).filter(Boolean).join(" ")); break; }
    case "ledniv": { const p = x.fm1_leds(); const l = mem().slice(p, p + 41);   // ledniv : every LED's level 0..255
                     console.log("ledniv", [...l].map((v, i) => (i < 14 ? Object.keys(BTN)[i] : "k" + (i - 14)) + "=" + v).join(" ")); break; }
    case "turn": x.fm1_encoder(ENC[a], Number(b)); run(60); break;
    case "shot": png(`${outDir}/${a}.png`); break;
    case "rec": if (a === "on") { audio.length = 0; recording = true; } else { recording = false; console.log("audio", JSON.stringify(wav(`${outDir}/audio.wav`))); } break;
    case "pcs": {
      const h = new Map();
      for (let i = 0; i < Number(a); i++) { run(1); const w = where(x.fm1_debug_pc()); h.set(w.replace(/\+0x[0-9a-f]+$/, ""), (h.get(w.replace(/\+0x[0-9a-f]+$/, "")) ?? 0) + 1); }
      console.log("pc:", JSON.stringify([...h].sort((p, q) => q[1] - p[1]).slice(0, 8)), "last", where(x.fm1_debug_pc()));
      break;
    }
    case "pcsx": {                           // pcsx N: like pcs, but each address (the hot instructions)
      const h = new Map();
      for (let i = 0; i < Number(a); i++) { run(1); const pc = x.fm1_debug_pc(); h.set(pc, (h.get(pc) ?? 0) + 1); }
      console.log("pcx:", [...h].sort((p, q) => q[1] - p[1]).slice(0, 40).map(([pc, n]) => `${pc.toString(16)}(${where(pc)})=${n}`).join(" "));
      break;
    }
    case "dump": {                           // dump ADDR LEN: guest memory bytes, in hex
      const ad = parseInt(a, 16), n = Number(b ?? 16), out = [];
      for (let i = 0; i < n; i++) { const w = x.fm1_debug_peek((ad + i) & ~3) >>> 0; out.push(((w >>> (8 * ((ad + i) & 3))) & 255).toString(16).padStart(2, "0")); }
      console.log("dump", a, out.join(" "));
      break;
    }
    case "peek": {                           // peek ADDR N: sample a register N times, 1 ms apart
      const vals = [];
      for (let i = 0; i < Number(b ?? 1); i++) { run(1); vals.push(x.fm1_debug_peek(parseInt(a, 16)).toString(16)); }
      console.log("peek", a, vals.join(" "));
      break;
    }
    case "lcd": {                            // lcd N: the panel's on / sleep flags and last commands, N samples
      const vals = [];
      for (let i = 0; i < Number(a ?? 1); i++) {
        run(1);
        const v = x.fm1_debug_lcd(), f = v % 256, c = Math.floor(v / 256);
        vals.push(`${f & 1 ? "on" : "OFF"}${f & 2 ? "/SLEEP" : ""}[${c.toString(16).padStart(8, "0")}]`);
      }
      console.log("lcd", vals.join(" "));
      break;
    }
    case "vis": {                            // vis N: the emulator's screen-visible flag, N samples
      const vals = [];
      for (let i = 0; i < Number(a ?? 1); i++) { run(1); x.fm1_status(); const j = JSON.parse(msg()); vals.push(j.visible ? 1 : 0); }
      console.log("visible", vals.join(""));
      break;
    }
    case "px": {                             // px N MS: every MS ms, the share of non-black pixels and pixels written
      const out = [];
      for (let i = 0; i < Number(a); i++) {
        run(Number(b ?? 50));
        const rgba = frame();
        let lit = 0;
        for (let k = 0; k < rgba.length; k += 4) if (rgba[k] | rgba[k + 1] | rgba[k + 2]) lit++;
        x.fm1_status(); const j = JSON.parse(msg());
        out.push(`${Math.round(lit / 576)}%/${j.lcdPixels}`);
      }
      console.log("px", out.join(" "));
      break;
    }
    case "dumpecran": {                      // dumpecran ADDR W H : rows' rightmost non-background x
      const base = parseInt(a, 16), W = Number(b), H = Number(raw.trim().split(/\s+/)[3]);
      const out = [];
      for (let y = 0; y < H; y++) {
        const row = [];
        for (let x = 0; x < W; x += 4) { const v = x.fm1_debug_peek ? 0 : 0; }
        let first = -1, last = -1, v0 = null;
        for (let x = 0; x < W; x += 4) {
          const w = x_peek(base + y * W + x);
          for (let k = 0; k < 4; k++) { const v = (w >>> (8 * k)) & 255; if (v0 === null) v0 = v; if (v !== v0) { if (first < 0) first = x + k; last = x + k; } }
        }
        if (last >= 0) out.push(`${y}:${first}-${last}`);
      }
      console.log("contenu", out.join(" "));
      break;
    }
    default: throw new Error("unknown command " + cmd);
  }
}
x.fm1_status();
const st = JSON.parse(msg());
console.log("ok", JSON.stringify({ guestSeconds: +st.guestSeconds.toFixed(2), fault: st.fault }));
if (process.env.STATUS) console.log(JSON.stringify(st));
