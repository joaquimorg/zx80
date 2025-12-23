#include <Arduino.h>

extern "C" {
#include "zx80_basic.h"
}

static zx80_basic_t vm;

static void serial_write_char(char c, void *user) {
  (void)user;
  Serial.write(static_cast<uint8_t>(c));
}

static int serial_break_check(void *user) {
  (void)user;
  if (Serial.available() == 0) {
    return 0;
  }
  int c = Serial.peek();
  if (c == 0x03) {
    Serial.read();
    return 1;
  }
  return 0;
}

static int serial_read_line_blocking(char *buf, size_t max_len, void *user) {
  (void)user;
  if (max_len == 0) {
    return -1;
  }
  size_t pos = 0;
  while (true) {
    while (Serial.available() == 0) {
      delay(1);
    }
    char c = static_cast<char>(Serial.read());
    if (c == '\r' || c == '\n') {
      if (pos == 0) {
        continue;
      }
      break;
    }
    if (pos + 1 < max_len) {
      buf[pos++] = c;
    }
  }
  buf[pos] = '\0';
  return static_cast<int>(pos);
}

static void setup_vm() {
  zx80_io_t io;
  io.write_char = serial_write_char;
  io.read_line = serial_read_line_blocking;
  io.break_check = serial_break_check;
  io.user = nullptr;
  zx80_basic_init_default(&vm, io);
  zx80_basic_reset(&vm);
}

static void print_prompt() {
  Serial.print("READY>");
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }
  delay(1000);
  Serial.println("ZX80 BASIC ready");
  Serial.println("(c) 2026 joaquim.org");
  Serial.println(""); 
  print_prompt();
  setup_vm();  
}

void loop() {
  static char line[128];
  static size_t pos = 0;

  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    Serial.write(c);
    if (c == '\r' || c == '\n') {
      if (pos == 0) {
        continue;
      }
      line[pos] = '\0';
      zx80_basic_handle_line(&vm, line);
      pos = 0;
      print_prompt();
      continue;
    }
    if (pos + 1 < sizeof(line)) {
      line[pos++] = c;
    }
  }
}
