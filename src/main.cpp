#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>

extern "C" {
#include "zx80_basic.h"
}

static const char *kWifiSsid = "joaquim_wifi";
static const char *kWifiPass = "mblack#2014";
static const char *kPrompt = ">";

static zx80_basic_t vm;
static WebServer server(80);
static String out_buffer;
static volatile bool break_requested = false;
static bool fs_ready = false;

static String normalize_filename(String name) {
  name.trim();
  if (name.isEmpty()) {
    return "";
  }
  if (name.indexOf("..") != -1 || name.indexOf('/') != -1 ||
      name.indexOf('\\') != -1) {
    return "";
  }
  if (name.length() > 32) {
    return "";
  }
  String upper = name;
  upper.toUpperCase();
  if (!upper.endsWith(".BAS")) {
    name += ".BAS";
  }
  return name;
}

static String extract_filename(const String &line, const char *keyword) {
  String trimmed = line;
  trimmed.trim();
  String upper = trimmed;
  upper.toUpperCase();
  size_t key_len = strlen(keyword);
  if (!upper.startsWith(keyword)) {
    return "";
  }
  String rest = trimmed.substring(key_len);
  rest.trim();
  if (rest.isEmpty()) {
    return "";
  }
  String name;
  if (rest.startsWith("\"")) {
    int end = rest.indexOf('"', 1);
    if (end <= 1) {
      return "";
    }
    name = rest.substring(1, end);
  } else {
    int space = rest.indexOf(' ');
    name = (space == -1) ? rest : rest.substring(0, space);
  }
  return normalize_filename(name);
}

static String capture_listing() {
  out_buffer = "";
  zx80_basic_list(&vm);
  String listing = out_buffer;
  out_buffer = "";
  return listing;
}

static bool save_program(const String &name) {
  if (!fs_ready) {
    return false;
  }
  String listing = capture_listing();
  String path = "/" + name;
  File file = LittleFS.open(path, "w");
  if (!file) {
    return false;
  }
  file.print(listing);
  file.close();
  return true;
}

static bool load_program(const String &name) {
  if (!fs_ready) {
    return false;
  }
  String path = "/" + name;
  File file = LittleFS.open(path, "r");
  if (!file) {
    return false;
  }
  zx80_basic_reset(&vm);
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.replace("\r", "");
    line.trim();
    if (line.isEmpty()) {
      continue;
    }
    zx80_basic_handle_line(&vm, line.c_str());
  }
  file.close();
  return true;
}

static String list_programs() {
  if (!fs_ready) {
    return "";
  }
  String names;
  File root = LittleFS.open("/");
  if (!root) {
    return "";
  }
  File file = root.openNextFile();
  while (file) {
    if (!file.isDirectory()) {
      String name = file.name();
      if (name.startsWith("/")) {
        name = name.substring(1);
      }
      names += name;
      names += "\n";
    }
    file = root.openNextFile();
  }
  return names;
}

static bool is_program_line(const String &line) {
  String trimmed = line;
  trimmed.trim();
  if (trimmed.isEmpty()) {
    return false;
  }
  char c = trimmed[0];
  return c >= '0' && c <= '9';
}

static bool handle_special_command(const String &line, String &response) {
  String trimmed = line;
  trimmed.trim();
  if (trimmed.isEmpty()) {
    response = "";
    return true;
  }
  if (is_program_line(trimmed)) {
    return false;
  }
  String upper = trimmed;
  upper.toUpperCase();
  if (upper.startsWith("SAVE")) {
    String name = extract_filename(trimmed, "SAVE");
    if (name.isEmpty()) {
      response = "ERR";
      return true;
    }
    response = save_program(name) ? "OK" : "ERR";
    return true;
  }
  if (upper.startsWith("LOAD")) {
    String name = extract_filename(trimmed, "LOAD");
    if (name.isEmpty()) {
      response = "ERR";
      return true;
    }
    response = load_program(name) ? "OK" : "ERR";
    return true;
  }
  return false;
}

static const char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="pt">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>ZX80 BASIC Web</title>
    <link rel="stylesheet" href="/styles.css" />
  </head>
  <body>
    <div class="page">
      <header class="topbar">
        <div class="brand">
          <span class="dot"></span>
          <div>
            <p class="title">ZX80 BASIC</p>
            <p class="subtitle">Terminal</p>
          </div>
        </div>
        <div class="status">
          <span id="status">offline</span>
        </div>
      </header>

      <main class="workspace">
        <section class="display">          
          <div class="crt">
            <canvas id="screen" aria-label="ZX80 display"></canvas>
            <div class="glow"></div>
            <div class="scanlines"></div>
            <div class="vignette"></div>
          </div>
        </section>
      </main>
    </div>

    <div id="modal" class="modal hidden" role="dialog" aria-modal="true">
      <div class="modal-content">
        <h3>LOAD</h3>
        <div id="file-list" class="file-list"></div>
        <div class="modal-actions">
          <button id="load-cancel" class="btn">Cancelar</button>
        </div>
      </div>
    </div>

    <script src="/app.js"></script>
  </body>
</html>
)HTML";

static const char kStylesCss[] PROGMEM = R"CSS(
:root {
  color-scheme: light;
  --bg: #0f1c1f;
  --panel: #172a2e;
  --panel-alt: #1d353b;
  --accent: #f2c14e;
  --accent-strong: #f28f3b;
  --ink: #e9f5f8;
  --muted: #9fc3cf;
  --screen-ink: #0c2323;
  --scanline: rgba(12, 35, 35, 0.14);
  --glow: rgba(242, 193, 78, 0.35);
}

* {
  box-sizing: border-box;
}

body {
  margin: 0;
  min-height: 100vh;
  font-family: "Courier New", "Lucida Console", monospace;
  background-color: #101010;
  color: var(--ink);
}

.page {
  max-width: 900px;
  margin: 0 auto;
  padding: 32px 24px 48px;
}

.topbar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
  padding: 16px 20px;
  background: linear-gradient(120deg, var(--panel), var(--panel-alt));
  border-radius: 16px;
  box-shadow: 0 12px 30px rgba(0, 0, 0, 0.25);
}

.brand {
  display: flex;
  align-items: center;
  gap: 14px;
}

.brand .dot {
  width: 18px;
  height: 18px;
  border-radius: 50%;
  background: var(--accent);
  box-shadow: 0 0 18px var(--glow);
}

.title {
  margin: 0;
  font-size: 20px;
  letter-spacing: 1px;
}

.subtitle {
  margin: 2px 0 0;
  font-size: 12px;
  color: var(--muted);
}

.status {
  font-size: 12px;
  color: var(--muted);
  letter-spacing: 2px;
  text-transform: uppercase;
}

.workspace {
  margin-top: 24px;
}

.panel-title {
  font-size: 12px;
  letter-spacing: 2px;
  text-transform: uppercase;
  color: var(--muted);
  margin-bottom: 12px;
}

.display {
  
}

.crt {
  position: relative;
  background: radial-gradient(circle at center, #cbdde0 0%, #aebcc1 65%, #92a1a6 100%);
  border-radius: 14px;
  padding: 24px;
  min-height: 420px;
  box-shadow: inset 0 0 28px rgba(0, 0, 0, 0.35),
    0 18px 40px rgba(0, 0, 0, 0.35);
  overflow: hidden;
}

#screen {
  display: block;
  width: 100%;
  height: auto;
  image-rendering: pixelated;
}

.glow,
.scanlines,
.vignette {
  position: absolute;
  inset: 0;
  pointer-events: none;
}

.glow {
  box-shadow: 0 0 50px rgba(242, 193, 78, 0.28);
}

.scanlines {
  background: repeating-linear-gradient(
    to bottom,
    transparent,
    transparent 2px,
    var(--scanline) 3px
  );
  mix-blend-mode: multiply;
  opacity: 0.6;
}

.vignette {
  box-shadow: inset 0 0 60px rgba(0, 0, 0, 0.35);
}

.modal {
  position: fixed;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgba(0, 0, 0, 0.5);
  z-index: 10;
}

.modal.hidden {
  display: none;
}

.modal-content {
  background: #122126;
  border-radius: 12px;
  padding: 16px;
  width: min(360px, 90vw);
  box-shadow: 0 12px 30px rgba(0, 0, 0, 0.35);
  color: var(--ink);
}

.modal-content h3 {
  margin: 0 0 12px;
  font-size: 14px;
  letter-spacing: 2px;
}

.file-list {
  display: grid;
  gap: 8px;
  max-height: 240px;
  overflow-y: auto;
}

.file-list button {
  text-align: left;
  background: #0f1c1f;
  border: 1px solid rgba(242, 193, 78, 0.3);
  color: var(--ink);
  padding: 6px 10px;
  border-radius: 8px;
  cursor: pointer;
}

.file-list button:hover {
  background: #1b2f35;
}

.modal-actions {
  margin-top: 12px;
  display: flex;
  justify-content: flex-end;
}
)CSS";

static const char kAppJs[] PROGMEM = R"JS(
const screen = document.getElementById("screen");
const statusEl = document.getElementById("status");
const modal = document.getElementById("modal");
const fileListEl = document.getElementById("file-list");
const loadCancelBtn = document.getElementById("load-cancel");
const SCREEN_WIDTH = 64;
const SCREEN_HEIGHT = 24;
const OUTPUT_HEIGHT = SCREEN_HEIGHT - 1;
const GLYPH_W = 5;
const GLYPH_H = 7;
const SCALE = 1;
const ctx = screen.getContext("2d");
let cellWidth = 8;
let cellHeight = 8;

let promptText = ">";
let inputBuffer = "";
const outputLines = Array.from({ length: OUTPUT_HEIGHT }, () => "");
let cursorLine = 0;
let cursorCol = 0;
let cursorVisible = true;
let modalOpen = false;

const GLYPHS = {
  " ": [0, 0, 0, 0, 0, 0, 0],
  "!": [0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04],
  "\"": [0x0a, 0x0a, 0x00, 0x00, 0x00, 0x00, 0x00],
  "'": [0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00],
  "(": [0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02],
  ")": [0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08],
  "*": [0x00, 0x0a, 0x04, 0x1f, 0x04, 0x0a, 0x00],
  "+": [0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00],
  ",": [0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x04],
  "-": [0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00],
  ".": [0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06],
  "/": [0x01, 0x02, 0x04, 0x08, 0x10, 0x00, 0x00],
  ":": [0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00],
  ";": [0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x02],
  "<": [0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02],
  "=": [0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00, 0x00],
  ">": [0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08],
  "?": [0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04],
  "0": [0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e],
  "1": [0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e],
  "2": [0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f],
  "3": [0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e],
  "4": [0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02],
  "5": [0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e],
  "6": [0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e],
  "7": [0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08],
  "8": [0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e],
  "9": [0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c],
  "A": [0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11],
  "B": [0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e],
  "C": [0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e],
  "D": [0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e],
  "E": [0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f],
  "F": [0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10],
  "G": [0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f],
  "H": [0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11],
  "I": [0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e],
  "J": [0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c],
  "K": [0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11],
  "L": [0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f],
  "M": [0x11, 0x1b, 0x15, 0x11, 0x11, 0x11, 0x11],
  "N": [0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11],
  "O": [0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e],
  "P": [0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10],
  "Q": [0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d],
  "R": [0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11],
  "S": [0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e],
  "T": [0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04],
  "U": [0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e],
  "V": [0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04],
  "W": [0x11, 0x11, 0x11, 0x11, 0x15, 0x1b, 0x11],
  "X": [0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11],
  "Y": [0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04],
  "Z": [0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f],
};

function setupCanvas() {
  cellWidth = (GLYPH_W + 1) * SCALE;
  cellHeight = (GLYPH_H + 1) * SCALE;
  screen.width = SCREEN_WIDTH * cellWidth;
  screen.height = SCREEN_HEIGHT * cellHeight;
}

function getGlyph(ch) {
  const key = ch.toUpperCase();
  return GLYPHS[key] || GLYPHS["?"];
}

function drawGlyph(ch, x, y) {
  const glyph = getGlyph(ch);
  if (!glyph) return;
  for (let row = 0; row < GLYPH_H; row += 1) {
    const bits = glyph[row];
    for (let col = 0; col < GLYPH_W; col += 1) {
      if (bits & (1 << (GLYPH_W - 1 - col))) {
        ctx.fillRect(
          x + col * SCALE,
          y + row * SCALE,
          SCALE,
          SCALE
        );
      }
    }
  }
}

function render() {
  const lines = outputLines.slice();
  const promptLine = (promptText + inputBuffer).slice(0, SCREEN_WIDTH);
  lines.push(promptLine);
  ctx.clearRect(0, 0, screen.width, screen.height);
  ctx.fillStyle = "#0c2323";
  for (let row = 0; row < lines.length; row += 1) {
    const text = (lines[row] || "").padEnd(SCREEN_WIDTH, " ");
    for (let col = 0; col < SCREEN_WIDTH; col += 1) {
      const ch = text[col];
      if (ch !== " ") {
        drawGlyph(ch, col * cellWidth, row * cellHeight);
      }
    }
  }
  if (cursorVisible) {
    const cursorColPos = Math.min(promptLine.length, SCREEN_WIDTH - 1);
    const cursorRowPos = SCREEN_HEIGHT - 1;
    ctx.fillStyle = "#0c2323";
    ctx.fillRect(
      cursorColPos * cellWidth,
      cursorRowPos * cellHeight,
      cellWidth,
      cellHeight
    );
  }
}

function openModal() {
  modal.classList.remove("hidden");
  modalOpen = true;
}

function closeModal() {
  modal.classList.add("hidden");
  modalOpen = false;
}

async function openLoadDialog() {
  openModal();
  fileListEl.innerHTML = "";
  try {
    const response = await fetch("/list");
    const text = await response.text();
    const names = text.split("\n").map((item) => item.trim()).filter(Boolean);
    if (!names.length) {
      const empty = document.createElement("div");
      empty.textContent = "Sem programas guardados.";
      fileListEl.appendChild(empty);
      return;
    }
    names.forEach((name) => {
      const button = document.createElement("button");
      button.textContent = name;
      button.addEventListener("click", () => {
        closeModal();
        loadProgram(name);
      });
      fileListEl.appendChild(button);
    });
  } catch (error) {
    const empty = document.createElement("div");
    empty.textContent = "Erro a ler programas.";
    fileListEl.appendChild(empty);
  }
}

async function loadProgram(name) {
  try {
    const response = await fetch(`/load?name=${encodeURIComponent(name)}`);
    const text = await response.text();
    const parsed = parseResponse(text);
    applyOutput(parsed.out || "");
    promptText = parsed.prompt || ">";
    statusEl.textContent = "online";
  } catch (error) {
    statusEl.textContent = "offline";
  }
}

function writeChar(ch) {
  if (ch === "\n") {
    cursorLine += 1;
    cursorCol = 0;
    if (cursorLine >= OUTPUT_HEIGHT) {
      outputLines.shift();
      outputLines.push("");
      cursorLine = OUTPUT_HEIGHT - 1;
    }
    return;
  }
  const line = outputLines[cursorLine] || "";
  const padded =
    line + " ".repeat(Math.max(0, cursorCol - line.length));
  const nextLine =
    padded.slice(0, cursorCol) + ch + padded.slice(cursorCol + 1);
  outputLines[cursorLine] = nextLine.slice(0, SCREEN_WIDTH);
  cursorCol += 1;
  if (cursorCol >= SCREEN_WIDTH) {
    writeChar("\n");
  }
}

function writeText(text) {
  for (const ch of text) {
    writeChar(ch);
  }
}

function printLine(text) {
  writeText(text);
  writeChar("\n");
}

function resetScreen() {
  for (let i = 0; i < outputLines.length; i += 1) {
    outputLines[i] = "";
  }
  cursorLine = 0;
  cursorCol = 0;
  inputBuffer = "";
  render();
}

function applyOutput(text) {
  const normalized = text.replace(/\r\n/g, "\n").replace(/\r/g, "\n");
  for (const ch of normalized) {
    if (ch === "\n") {
      writeChar("\n");
    } else {
      writeChar(ch);
    }
  }
  render();
}

function pushInputLine(line) {
  printLine(promptText + line);
  inputBuffer = "";
  render();
}

async function boot() {
  try {
    const response = await fetch("/boot");
    const text = await response.text();
    const parsed = parseResponse(text);
    applyOutput(parsed.out || "");
    promptText = parsed.prompt || ">";
    statusEl.textContent = "online";
  } catch (error) {
    statusEl.textContent = "offline";
  }
}

async function sendLine(line) {
  try {
    const response = await fetch("/line", {
      method: "POST",
      headers: { "Content-Type": "text/plain" },
      body: line,
    });
    const text = await response.text();
    const parsed = parseResponse(text);
    applyOutput(parsed.out || "");
    promptText = parsed.prompt || ">";
    statusEl.textContent = "online";
  } catch (error) {
    statusEl.textContent = "offline";
  }
}

function parseResponse(text) {
  let prompt = ">";
  let out = text || "";
  if (out.startsWith("PROMPT:")) {
    const lines = out.split("\n");
    if (lines.length >= 2) {
      prompt = lines[0].slice("PROMPT:".length) || ">";
      if (lines[1].startsWith("DATA:")) {
        out = lines.slice(2).join("\n");
      }
    }
  }
  out = out
    .replace(/\\\\r\\\\n/g, "\n")
    .replace(/\\\\n/g, "\n")
    .replace(/\\\\r/g, "\n")
    .replace(/\\r\\n/g, "\n")
    .replace(/\\n/g, "\n")
    .replace(/\\r/g, "\n");
  return { out, prompt };
}

document.addEventListener("keydown", (event) => {
  if (modalOpen) {
    if (event.key === "Escape") {
      event.preventDefault();
      closeModal();
    }
    return;
  }
  if (event.ctrlKey && (event.key === "c" || event.key === "C")) {
    event.preventDefault();
    fetch("/break", { method: "POST" });
    return;
  }
  if (event.key === "Enter") {
    event.preventDefault();
    const line = inputBuffer;
    pushInputLine(line);
    const trimmed = line.trim();
    const upper = trimmed.toUpperCase();
    if (upper === "LOAD") {
      openLoadDialog();
    } else if (upper.startsWith("LOAD ")) {
      const name = trimmed.slice(4).trim().replace(/^\"|\"$/g, "");
      loadProgram(name);
    } else {
      sendLine(line);
    }
    return;
  }
  if (event.key === "Backspace") {
    event.preventDefault();
    inputBuffer = inputBuffer.slice(0, -1);
    render();
    return;
  }
  if (event.key.length === 1) {
    if (inputBuffer.length < SCREEN_WIDTH - promptText.length) {
      inputBuffer += event.key;
      render();
    }
  }
});

loadCancelBtn.addEventListener("click", () => {
  closeModal();
});

setupCanvas();
resetScreen();
boot();
setInterval(() => {
  cursorVisible = !cursorVisible;
  render();
}, 500);
)JS";

static void web_write_char(char c, void *user) {
  (void)user;
  out_buffer += c;
}

static int web_break_check(void *user) {
  (void)user;
  if (break_requested) {
    break_requested = false;
    return 1;
  }
  return 0;
}

static int web_read_line(char *buf, size_t max_len, void *user) {
  (void)user;
  (void)buf;
  (void)max_len;
  return -1;
}

static void setup_vm() {
  zx80_io_t io;
  io.write_char = web_write_char;
  io.read_line = web_read_line;
  io.break_check = web_break_check;
  io.user = nullptr;
  zx80_basic_init_default(&vm, io);
  zx80_basic_reset(&vm);
}

static void send_response(const String &out) {
  String payload = String("PROMPT:") + kPrompt + "\nDATA:\n" + out;
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", payload);
}

static String handle_line(const String &line) {
  String response;
  if (handle_special_command(line, response)) {
    return response;
  }
  out_buffer = "";
  zx80_basic_handle_line(&vm, line.c_str());
  return out_buffer;
}

static void setup_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(kWifiSsid, kWifiPass);
  unsigned long start = millis();
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(200);
    if (millis() - start > 15000) {
      break;
    }
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Web terminal: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connect failed");
  }
}

static void setup_web() {
  server.on("/", []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", kIndexHtml);
  });
  server.on("/styles.css", []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/css", kStylesCss);
  });
  server.on("/app.js", []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "application/javascript", kAppJs);
  });
  server.on("/boot", HTTP_GET, []() {
    String banner = String("ZX80 BASIC ready\n(c) 2026 joaquim.org\n\n");
    send_response(banner);
  });
  server.on("/list", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain", list_programs());
  });
  server.on("/load", HTTP_GET, []() {
    String name = normalize_filename(server.arg("name"));
    if (name.isEmpty()) {
      send_response("ERR");
      return;
    }
    send_response(load_program(name) ? "OK" : "ERR");
  });
  server.on("/line", HTTP_POST, []() {
    String line = server.arg("plain");
    send_response(handle_line(line));
  });
  server.on("/break", HTTP_POST, []() {
    break_requested = true;
    send_response("");
  });
  server.begin();
}

void setup() {
  Serial.begin(115200);
  fs_ready = LittleFS.begin(true);
  if (!fs_ready) {
    Serial.println("LittleFS mount failed");
  }
  setup_vm();
  setup_wifi();
  setup_web();
}

void loop() {
  server.handleClient();
}
