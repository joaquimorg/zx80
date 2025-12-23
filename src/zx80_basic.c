// ZX80 BASIC minimal interpreter core (C)
#include "zx80_basic.h"

#include <ctype.h>
#include <string.h>

static uint8_t default_ram[ZX80_BASIC_DEFAULT_RAM];
static uint8_t default_array_mem[ZX80_BASIC_DEFAULT_ARRAY_MEM];

static void write_char(zx80_basic_t *vm, char c) {
  if (vm->io.write_char) {
    vm->io.write_char(c, vm->io.user);
  }
}

static void write_str(zx80_basic_t *vm, const char *s) {
  while (s && *s) {
    write_char(vm, *s++);
  }
}

static void write_int(zx80_basic_t *vm, zx80_int v) {
  char buf[16];
  int pos = 0;
  if (v == 0) {
    buf[pos++] = '0';
  } else {
    if (v < 0) {
      buf[pos++] = '-';
      v = -v;
    }
    char tmp[12];
    int tpos = 0;
    while (v > 0 && tpos < (int)sizeof(tmp)) {
      tmp[tpos++] = (char)('0' + (v % 10));
      v /= 10;
    }
    while (tpos > 0) {
      buf[pos++] = tmp[--tpos];
    }
  }
  buf[pos] = '\0';
  write_str(vm, buf);
}

static void write_newline(zx80_basic_t *vm) {
  write_char(vm, '\r');
  write_char(vm, '\n');
}

static uint16_t read_u16(const uint8_t *p) {
  return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static void write_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static const char *skip_ws(const char *s) {
  while (*s && isspace((unsigned char)*s)) {
    s++;
  }
  return s;
}

static int is_name_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static const char *match_kw(const char *s, const char *kw) {
  const char *p = s;
  while (*kw) {
    char a = *p++;
    if (!a) {
      return NULL;
    }
    if (a >= 'a' && a <= 'z') {
      a = (char)(a - ('a' - 'A'));
    }
    if (a != *kw++) {
      return NULL;
    }
  }
  if (is_name_char(*p)) {
    return NULL;
  }
  return p;
}

static const char *parse_int(const char *s, zx80_int *out) {
  s = skip_ws(s);
  int sign = 1;
  if (*s == '-') {
    sign = -1;
    s++;
  } else if (*s == '+') {
    s++;
  }
  if (!isdigit((unsigned char)*s)) {
    return NULL;
  }
  zx80_int v = 0;
  while (isdigit((unsigned char)*s)) {
    v = (v * 10) + (*s - '0');
    s++;
  }
  *out = v * sign;
  return s;
}

static const char *parse_var(const char *s, int *out_index) {
  s = skip_ws(s);
  if (!is_name_char(*s)) {
    return NULL;
  }
  char c = (char)toupper((unsigned char)*s);
  if (c < 'A' || c > 'Z') {
    return NULL;
  }
  *out_index = c - 'A';
  return s + 1;
}

static const char *parse_expr(zx80_basic_t *vm, const char *s, zx80_int *out);

static zx80_array_t *find_array(zx80_basic_t *vm, int var) {
  for (int i = 0; i < vm->array_count; ++i) {
    if (vm->arrays[i].var == var) {
      return &vm->arrays[i];
    }
  }
  return NULL;
}

static zx80_int *array_at(zx80_basic_t *vm, zx80_array_t *arr, zx80_int i,
                          zx80_int j) {
  if (!arr || !vm->array_mem) {
    return NULL;
  }
  if (i < 0 || j < 0) {
    return NULL;
  }
  if (i > arr->size1 || j > arr->size2) {
    return NULL;
  }
  size_t stride = (size_t)(arr->size1 + 1);
  size_t index = (size_t)i + stride * (size_t)j;
  zx80_int *base = (zx80_int *)(vm->array_mem + arr->offset);
  return &base[index];
}

static const char *parse_indices(zx80_basic_t *vm, const char *s, zx80_int *i,
                                 zx80_int *j, int *dims) {
  s = skip_ws(s);
  if (*s != '(') {
    return NULL;
  }
  s++;
  s = parse_expr(vm, s, i);
  if (!s) {
    return NULL;
  }
  s = skip_ws(s);
  *dims = 1;
  *j = 0;
  if (*s == ',') {
    s++;
    s = parse_expr(vm, s, j);
    if (!s) {
      return NULL;
    }
    *dims = 2;
  }
  s = skip_ws(s);
  if (*s != ')') {
    return NULL;
  }
  return s + 1;
}

static size_t align_up(size_t v, size_t a) {
  return (v + (a - 1)) & ~(a - 1);
}

static zx80_int rand_next(zx80_basic_t *vm, zx80_int range) {
  if (range <= 0) {
    return 0;
  }
  vm->rand_state = (uint32_t)(vm->rand_state * 1103515245u + 12345u);
  return (zx80_int)((vm->rand_state % (uint32_t)range) + 1);
}

static const char *parse_factor(zx80_basic_t *vm, const char *s,
                                zx80_int *out) {
  s = skip_ws(s);
  if (*s == '(') {
    s++;
    s = parse_expr(vm, s, out);
    if (!s) {
      return NULL;
    }
    s = skip_ws(s);
    if (*s != ')') {
      return NULL;
    }
    return s + 1;
  }
  if (*s == '+' || *s == '-') {
    char sign = *s++;
    s = parse_factor(vm, s, out);
    if (!s) {
      return NULL;
    }
    if (sign == '-') {
      *out = -*out;
    }
    return s;
  }
  const char *kw = match_kw(s, "RND");
  if (kw) {
    s = kw;
    s = skip_ws(s);
    if (*s != '(') {
      return NULL;
    }
    s++;
    zx80_int range = 0;
    s = parse_expr(vm, s, &range);
    if (!s) {
      return NULL;
    }
    s = skip_ws(s);
    if (*s != ')') {
      return NULL;
    }
    *out = rand_next(vm, range);
    return s + 1;
  }
  kw = match_kw(s, "PEEK");
  if (kw) {
    s = kw;
    s = skip_ws(s);
    if (*s != '(') {
      return NULL;
    }
    s++;
    zx80_int addr = 0;
    s = parse_expr(vm, s, &addr);
    if (!s) {
      return NULL;
    }
    s = skip_ws(s);
    if (*s != ')') {
      return NULL;
    }
    if (addr < 0 || (size_t)addr >= vm->ram_size) {
      *out = 0;
    } else {
      *out = vm->ram[addr];
    }
    return s + 1;
  }
  if (is_name_char(*s)) {
    int idx = 0;
    s = parse_var(s, &idx);
    if (!s) {
      return NULL;
    }
    const char *ns = skip_ws(s);
    if (*ns == '(') {
      zx80_int i = 0;
      zx80_int j = 0;
      int dims = 0;
      ns = parse_indices(vm, ns, &i, &j, &dims);
      if (!ns) {
        return NULL;
      }
      zx80_array_t *arr = find_array(vm, idx);
      if (!arr || arr->dims != dims) {
        return NULL;
      }
      zx80_int *cell = array_at(vm, arr, i, j);
      if (!cell) {
        return NULL;
      }
      *out = *cell;
      return ns;
    }
    *out = vm->vars[idx];
    return s;
  }
  return parse_int(s, out);
}

static const char *parse_term(zx80_basic_t *vm, const char *s, zx80_int *out) {
  s = parse_factor(vm, s, out);
  if (!s) {
    return NULL;
  }
  while (1) {
    s = skip_ws(s);
    if (*s != '*' && *s != '/') {
      break;
    }
    char op = *s++;
    zx80_int rhs = 0;
    s = parse_factor(vm, s, &rhs);
    if (!s) {
      return NULL;
    }
    if (op == '*') {
      *out = (*out) * rhs;
    } else {
      if (rhs == 0) {
        *out = 0;
      } else {
        *out = (*out) / rhs;
      }
    }
  }
  return s;
}

static const char *parse_arith(zx80_basic_t *vm, const char *s,
                               zx80_int *out) {
  s = parse_term(vm, s, out);
  if (!s) {
    return NULL;
  }
  while (1) {
    s = skip_ws(s);
    if (*s != '+' && *s != '-') {
      break;
    }
    char op = *s++;
    zx80_int rhs = 0;
    s = parse_term(vm, s, &rhs);
    if (!s) {
      return NULL;
    }
    if (op == '+') {
      *out = (*out) + rhs;
    } else {
      *out = (*out) - rhs;
    }
  }
  return s;
}

static const char *parse_expr(zx80_basic_t *vm, const char *s, zx80_int *out) {
  s = parse_arith(vm, s, out);
  if (!s) {
    return NULL;
  }
  s = skip_ws(s);
  if (*s != '<' && *s != '>' && *s != '=') {
    return s;
  }
  char op1 = *s++;
  char op2 = '\0';
  if ((op1 == '<' || op1 == '>') && (*s == '=' || *s == '>')) {
    op2 = *s++;
  }
  zx80_int rhs = 0;
  s = parse_arith(vm, s, &rhs);
  if (!s) {
    return NULL;
  }
  int result = 0;
  if (op1 == '=') {
    result = (*out == rhs);
  } else if (op1 == '<' && op2 == '>') {
    result = (*out != rhs);
  } else if (op1 == '<' && op2 == '=') {
    result = (*out <= rhs);
  } else if (op1 == '>' && op2 == '=') {
    result = (*out >= rhs);
  } else if (op1 == '<') {
    result = (*out < rhs);
  } else if (op1 == '>') {
    result = (*out > rhs);
  }
  *out = result ? -1 : 0;
  return s;
}

static uint8_t *find_line(zx80_basic_t *vm, uint16_t line,
                          uint8_t **out_prev) {
  uint8_t *p = vm->ram;
  uint8_t *prev = NULL;
  while (p < vm->ram + vm->prog_end) {
    uint16_t ln = read_u16(p);
    uint16_t len = read_u16(p + 2);
    if (ln == line) {
      if (out_prev) {
        *out_prev = prev;
      }
      return p;
    }
    if (ln > line) {
      break;
    }
    prev = p;
    p += 4 + len;
  }
  if (out_prev) {
    *out_prev = prev;
  }
  return NULL;
}

static uint8_t *find_insert_pos(zx80_basic_t *vm, uint16_t line) {
  uint8_t *p = vm->ram;
  while (p < vm->ram + vm->prog_end) {
    uint16_t ln = read_u16(p);
    uint16_t len = read_u16(p + 2);
    if (ln > line) {
      return p;
    }
    p += 4 + len;
  }
  return p;
}

static int delete_line(zx80_basic_t *vm, uint16_t line) {
  uint8_t *line_ptr = find_line(vm, line, NULL);
  if (!line_ptr) {
    return 0;
  }
  uint16_t len = read_u16(line_ptr + 2);
  uint8_t *next = line_ptr + 4 + len;
  size_t tail = (size_t)(vm->ram + vm->prog_end - next);
  memmove(line_ptr, next, tail);
  vm->prog_end -= (4 + len);
  return 1;
}

static int insert_line(zx80_basic_t *vm, uint16_t line, const char *text,
                       size_t text_len) {
  delete_line(vm, line);
  size_t need = 4 + text_len;
  if (vm->prog_end + need > vm->ram_size) {
    return -1;
  }
  uint8_t *pos = find_insert_pos(vm, line);
  size_t tail = (size_t)(vm->ram + vm->prog_end - pos);
  memmove(pos + need, pos, tail);
  write_u16(pos, line);
  write_u16(pos + 2, (uint16_t)text_len);
  memcpy(pos + 4, text, text_len);
  vm->prog_end += need;
  return 0;
}

static void list_program(zx80_basic_t *vm) {
  uint8_t *p = vm->ram;
  while (p < vm->ram + vm->prog_end) {
    uint16_t ln = read_u16(p);
    uint16_t len = read_u16(p + 2);
    write_int(vm, ln);
    write_char(vm, ' ');
    for (uint16_t i = 0; i < len; ++i) {
      write_char(vm, (char)p[4 + i]);
    }
    write_newline(vm);
    p += 4 + len;
  }
}

static void handle_error(zx80_basic_t *vm, const char *msg) {
  write_str(vm, msg);
  write_newline(vm);
}

static int exec_statement(zx80_basic_t *vm, const char *s,
                          const uint8_t *current_line,
                          const uint8_t *next_line,
                          const uint8_t **jump_ptr, uint16_t *jump_line,
                          int *stop);

static int exec_print(zx80_basic_t *vm, const char *s) {
  s = skip_ws(s);
  if (*s == '\0') {
    write_newline(vm);
    return 0;
  }
  int suppress_nl = 0;
  while (*s) {
    s = skip_ws(s);
    if (*s == '"') {
      s++;
      while (*s && *s != '"') {
        write_char(vm, *s++);
      }
      if (*s == '"') {
        s++;
      }
    } else {
      zx80_int v = 0;
      const char *ns = parse_expr(vm, s, &v);
      if (!ns) {
        return -1;
      }
      write_int(vm, v);
      s = ns;
    }
    s = skip_ws(s);
    if (*s == ';') {
      suppress_nl = 1;
      s++;
      continue;
    }
    if (*s == ',') {
      suppress_nl = 0;
      write_char(vm, ' ');
      s++;
      continue;
    }
    break;
  }
  if (!suppress_nl) {
    write_newline(vm);
  }
  return 0;
}

static int exec_let(zx80_basic_t *vm, const char *s) {
  int idx = 0;
  s = parse_var(s, &idx);
  if (!s) {
    return -1;
  }
  s = skip_ws(s);
  zx80_int i = 0;
  zx80_int j = 0;
  int dims = 0;
  int is_array = 0;
  if (*s == '(') {
    s = parse_indices(vm, s, &i, &j, &dims);
    if (!s) {
      return -1;
    }
    is_array = 1;
    s = skip_ws(s);
  }
  if (*s != '=') {
    return -1;
  }
  s++;
  zx80_int v = 0;
  s = parse_expr(vm, s, &v);
  if (!s) {
    return -1;
  }
  if (!is_array) {
    vm->vars[idx] = v;
    return 0;
  }
  zx80_array_t *arr = find_array(vm, idx);
  if (!arr || arr->dims != dims) {
    return -1;
  }
  zx80_int *cell = array_at(vm, arr, i, j);
  if (!cell) {
    return -1;
  }
  *cell = v;
  return 0;
}

static int exec_input(zx80_basic_t *vm, const char *s) {
  int idx = 0;
  s = parse_var(s, &idx);
  if (!s) {
    return -1;
  }
  write_str(vm, "? ");
  if (!vm->io.read_line) {
    return -1;
  }
  char buf[64];
  int len = vm->io.read_line(buf, sizeof(buf), vm->io.user);
  if (len <= 0) {
    return -1;
  }
  buf[sizeof(buf) - 1] = '\0';
  zx80_int v = 0;
  if (!parse_int(buf, &v)) {
    v = 0;
  }
  vm->vars[idx] = v;
  return 0;
}

static int exec_if(zx80_basic_t *vm, const char *s,
                   const uint8_t *current_line, const uint8_t *next_line,
                   const uint8_t **jump_ptr, uint16_t *jump_line, int *stop) {
  zx80_int cond = 0;
  s = parse_expr(vm, s, &cond);
  if (!s) {
    return -1;
  }
  s = skip_ws(s);
  const char *kw = match_kw(s, "THEN");
  if (!kw) {
    return -1;
  }
  s = kw;
  s = skip_ws(s);
  if (cond == 0) {
    return 0;
  }
  if (isdigit((unsigned char)*s)) {
    zx80_int line = 0;
    const char *ns = parse_int(s, &line);
    if (!ns) {
      return -1;
    }
    if (line < 0 || line > 65535) {
      return -1;
    }
    *jump_line = (uint16_t)line;
    return 0;
  }
  return exec_statement(vm, s, current_line, next_line, jump_ptr, jump_line,
                        stop);
}

static int exec_statement(zx80_basic_t *vm, const char *s,
                          const uint8_t *current_line,
                          const uint8_t *next_line,
                          const uint8_t **jump_ptr, uint16_t *jump_line,
                          int *stop) {
  (void)current_line;
  s = skip_ws(s);
  if (*s == '\0') {
    return 0;
  }
  const char *kw = match_kw(s, "REM");
  if (kw) {
    return 0;
  }
  kw = match_kw(s, "PRINT");
  if (kw) {
    s = kw;
    return exec_print(vm, s);
  }
  kw = match_kw(s, "LET");
  if (kw) {
    s = kw;
    return exec_let(vm, s);
  }
  kw = match_kw(s, "INPUT");
  if (kw) {
    s = kw;
    return exec_input(vm, s);
  }
  kw = match_kw(s, "GOTO");
  if (kw) {
    s = kw;
    zx80_int line = 0;
    s = parse_int(s, &line);
    if (!s) {
      return -1;
    }
    if (line < 0 || line > 65535) {
      return -1;
    }
    *jump_line = (uint16_t)line;
    return 0;
  }
  kw = match_kw(s, "IF");
  if (kw) {
    s = kw;
    return exec_if(vm, s, current_line, next_line, jump_ptr, jump_line, stop);
  }
  kw = match_kw(s, "END");
  if (kw) {
    *stop = 1;
    vm->cont_ptr = NULL;
    return 0;
  }
  kw = match_kw(s, "STOP");
  if (kw) {
    *stop = 1;
    if (next_line) {
      vm->cont_ptr = next_line;
    }
    return 0;
  }
  kw = match_kw(s, "RUN");
  if (kw) {
    s = kw;
    s = skip_ws(s);
    if (*s) {
      zx80_int line = 0;
      s = parse_int(s, &line);
      if (!s || line < 0 || line > 65535) {
        return -1;
      }
      *jump_line = (uint16_t)line;
    } else {
      *jump_line = 0xFFFF;
    }
    return 1;
  }
  kw = match_kw(s, "LIST");
  if (kw) {
    list_program(vm);
    return 0;
  }
  kw = match_kw(s, "NEW");
  if (kw) {
    zx80_basic_reset(vm);
    return 0;
  }
  kw = match_kw(s, "CLS");
  if (kw) {
    for (int i = 0; i < 8; ++i) {
      write_newline(vm);
    }
    return 0;
  }
  kw = match_kw(s, "CONTINUE");
  if (!kw) {
    kw = match_kw(s, "CONT");
  }
  if (kw) {
    if (!vm->cont_ptr) {
      return -1;
    }
    if (jump_ptr) {
      *jump_ptr = vm->cont_ptr;
    }
    return 0;
  }
  kw = match_kw(s, "GOSUB");
  if (kw) {
    if (!next_line) {
      return -1;
    }
    s = kw;
    zx80_int line = 0;
    s = parse_int(s, &line);
    if (!s || line < 0 || line > 65535) {
      return -1;
    }
    if (vm->gosub_sp >= ZX80_BASIC_GOSUB_DEPTH) {
      return -1;
    }
    vm->gosub_stack[vm->gosub_sp++] = next_line;
    *jump_line = (uint16_t)line;
    return 0;
  }
  kw = match_kw(s, "RETURN");
  if (kw) {
    if (vm->gosub_sp <= 0) {
      return -1;
    }
    if (jump_ptr) {
      *jump_ptr = vm->gosub_stack[--vm->gosub_sp];
    }
    return 0;
  }
  kw = match_kw(s, "FOR");
  if (kw) {
    if (!next_line) {
      return -1;
    }
    s = kw;
    int idx = 0;
    s = parse_var(s, &idx);
    if (!s) {
      return -1;
    }
    s = skip_ws(s);
    if (*s != '=') {
      return -1;
    }
    s++;
    zx80_int start = 0;
    s = parse_expr(vm, s, &start);
    if (!s) {
      return -1;
    }
    s = skip_ws(s);
    const char *to_kw = match_kw(s, "TO");
    if (!to_kw) {
      return -1;
    }
    s = to_kw;
    zx80_int end = 0;
    s = parse_expr(vm, s, &end);
    if (!s) {
      return -1;
    }
    zx80_int step = 1;
    s = skip_ws(s);
    kw = match_kw(s, "STEP");
    if (kw) {
      s = kw;
      s = parse_expr(vm, s, &step);
      if (!s) {
        return -1;
      }
    }
    if (vm->for_sp >= ZX80_BASIC_FOR_DEPTH) {
      return -1;
    }
    vm->vars[idx] = start;
    int run = (step >= 0) ? (start <= end) : (start >= end);
    if (!run) {
      const uint8_t *scan = next_line;
      int depth = 0;
      while (scan && scan < vm->ram + vm->prog_end) {
        uint16_t slen = read_u16(scan + 2);
        const char *text = (const char *)(scan + 4);
        const char *ts = skip_ws(text);
        if (match_kw(ts, "FOR")) {
          depth++;
        } else if (match_kw(ts, "NEXT")) {
          ts = match_kw(ts, "NEXT");
          ts = skip_ws(ts);
          int nidx = -1;
          if (*ts) {
            if (!parse_var(ts, &nidx)) {
              return -1;
            }
          }
          if (depth == 0 && (nidx < 0 || nidx == idx)) {
            if (jump_ptr) {
              *jump_ptr = scan + 4 + slen;
            }
            return 0;
          }
          if (depth > 0) {
            depth--;
          }
        }
        scan += 4 + slen;
      }
      return -1;
    }
    vm->for_stack[vm->for_sp].var = idx;
    vm->for_stack[vm->for_sp].end = end;
    vm->for_stack[vm->for_sp].step = step;
    vm->for_stack[vm->for_sp].line_ptr = next_line;
    vm->for_sp++;
    return 0;
  }
  kw = match_kw(s, "NEXT");
  if (kw) {
    if (vm->for_sp <= 0) {
      return -1;
    }
    s = kw;
    s = skip_ws(s);
    int idx = -1;
    if (*s) {
      s = parse_var(s, &idx);
      if (!s) {
        return -1;
      }
    }
    zx80_for_frame_t *frame = &vm->for_stack[vm->for_sp - 1];
    if (idx >= 0 && frame->var != idx) {
      return -1;
    }
    vm->vars[frame->var] += frame->step;
    zx80_int v = vm->vars[frame->var];
    int cont = (frame->step >= 0) ? (v <= frame->end) : (v >= frame->end);
    if (cont) {
      if (jump_ptr) {
        *jump_ptr = frame->line_ptr;
      }
    } else {
      vm->for_sp--;
    }
    return 0;
  }
  kw = match_kw(s, "POKE");
  if (kw) {
    s = kw;
    zx80_int addr = 0;
    s = parse_expr(vm, s, &addr);
    if (!s) {
      return -1;
    }
    s = skip_ws(s);
    if (*s != ',') {
      return -1;
    }
    s++;
    zx80_int value = 0;
    s = parse_expr(vm, s, &value);
    if (!s) {
      return -1;
    }
    if (addr >= 0 && (size_t)addr < vm->ram_size) {
      vm->ram[addr] = (uint8_t)(value & 0xFF);
    }
    return 0;
  }
  kw = match_kw(s, "RANDOMISE");
  if (!kw) {
    kw = match_kw(s, "RAND");
  }
  if (kw) {
    s = kw;
    s = skip_ws(s);
    if (*s) {
      zx80_int seed = 0;
      s = parse_expr(vm, s, &seed);
      if (!s) {
        return -1;
      }
      vm->rand_state = (uint32_t)seed;
    } else {
      vm->rand_state = (uint32_t)(vm->prog_end + 1);
    }
    return 0;
  }
  kw = match_kw(s, "DIM");
  if (kw) {
    s = kw;
    while (1) {
      int idx = 0;
      s = parse_var(s, &idx);
      if (!s) {
        return -1;
      }
      zx80_int size1 = 0;
      zx80_int size2 = 0;
      int dims = 0;
      s = parse_indices(vm, s, &size1, &size2, &dims);
      if (!s) {
        return -1;
      }
      if (size1 < 0 || size2 < 0) {
        return -1;
      }
      zx80_array_t *arr = find_array(vm, idx);
      if (!arr) {
        if (vm->array_count >= ZX80_BASIC_MAX_ARRAYS) {
          return -1;
        }
        arr = &vm->arrays[vm->array_count++];
        memset(arr, 0, sizeof(*arr));
        arr->var = idx;
      } else if (arr->dims != dims || arr->size1 != size1 ||
                 arr->size2 != (dims == 2 ? size2 : 0)) {
        return -1;
      }
      arr->dims = dims;
      arr->size1 = size1;
      arr->size2 = (dims == 2) ? size2 : 0;
      if (!vm->array_mem || vm->array_mem_size == 0) {
        return -1;
      }
      size_t count = (size_t)(size1 + 1) * (size_t)(arr->size2 + 1);
      size_t need = count * sizeof(zx80_int);
      if (arr->bytes == 0) {
        size_t start = align_up(vm->array_mem_used, sizeof(zx80_int));
        if (start + need > vm->array_mem_size) {
          return -1;
        }
        arr->offset = start;
        arr->bytes = need;
        vm->array_mem_used = start + need;
      } else if (arr->bytes != need) {
        return -1;
      }
      memset(vm->array_mem + arr->offset, 0, arr->bytes);
      s = skip_ws(s);
      if (*s != ',') {
        break;
      }
      s++;
    }
    return 0;
  }
  kw = match_kw(s, "LOAD");
  if (!kw) {
    kw = match_kw(s, "SAVE");
  }
  if (kw) {
    return 0;
  }

  if (is_name_char(*s)) {
    const char *p = s;
    int idx = 0;
    p = parse_var(p, &idx);
    if (p) {
      const char *q = skip_ws(p);
      if (q[0] == '=' || q[0] == '(') {
        return exec_let(vm, s);
      }
    }
  }
  return -1;
}

void zx80_basic_init(zx80_basic_t *vm, uint8_t *ram, size_t ram_size,
                     zx80_io_t io) {
  memset(vm, 0, sizeof(*vm));
  vm->ram = ram;
  vm->ram_size = ram_size;
  vm->io = io;
  vm->rand_state = 1;
  vm->array_mem = NULL;
  vm->array_mem_size = 0;
  vm->array_mem_used = 0;
  vm->array_count = 0;
}

void zx80_basic_init_default(zx80_basic_t *vm, zx80_io_t io) {
  zx80_basic_init(vm, default_ram, sizeof(default_ram), io);
  vm->array_mem = default_array_mem;
  vm->array_mem_size = sizeof(default_array_mem);
}

void zx80_basic_reset(zx80_basic_t *vm) {
  vm->prog_end = 0;
  memset(vm->vars, 0, sizeof(vm->vars));
  vm->gosub_sp = 0;
  vm->for_sp = 0;
  vm->cont_ptr = NULL;
  vm->rand_state = 1;
  vm->array_count = 0;
  vm->array_mem_used = 0;
}

void zx80_basic_list(zx80_basic_t *vm) {
  list_program(vm);
}

static int exec_program_from(zx80_basic_t *vm, uint8_t *start_pc) {
  vm->cont_ptr = NULL;
  vm->gosub_sp = 0;
  vm->for_sp = 0;
  uint8_t *pc = start_pc;
  while (pc < vm->ram + vm->prog_end) {
    if (vm->io.break_check && vm->io.break_check(vm->io.user)) {
      uint16_t len = read_u16(pc + 2);
      vm->cont_ptr = pc + 4 + len;
      write_str(vm, "BREAK");
      write_newline(vm);
      return 0;
    }
    uint16_t line = read_u16(pc);
    uint16_t len = read_u16(pc + 2);
    const char *text = (const char *)(pc + 4);

    uint16_t jump_line = 0xFFFF;
    const uint8_t *jump_ptr = NULL;
    int stop = 0;
    uint8_t *next_line = pc + 4 + len;
    int res = exec_statement(vm, text, pc, next_line, &jump_ptr, &jump_line,
                             &stop);
    if (res < 0) {
      write_str(vm, "ERROR IN ");
      write_int(vm, line);
      write_newline(vm);
      return -1;
    }
    if (stop) {
      return 0;
    }
    if (res == 1) {
      if (jump_line != 0xFFFF) {
        uint8_t *target = find_line(vm, jump_line, NULL);
        if (!target) {
          handle_error(vm, "LINE NOT FOUND");
          return -1;
        }
        pc = target;
      } else {
        pc = vm->ram;
      }
      continue;
    }
    if (jump_ptr) {
      pc = (uint8_t *)jump_ptr;
      continue;
    }
    if (jump_line != 0xFFFF) {
      uint8_t *target = find_line(vm, jump_line, NULL);
      if (!target) {
        handle_error(vm, "LINE NOT FOUND");
        return -1;
      }
      pc = target;
      continue;
    }

    pc += 4 + len;
  }
  return 0;
}

int zx80_basic_run(zx80_basic_t *vm) {
  return exec_program_from(vm, vm->ram);
}

int zx80_basic_handle_line(zx80_basic_t *vm, const char *line) {
  if (!line) {
    return 0;
  }
  const char *s = skip_ws(line);
  if (*s == '\0') {
    return 0;
  }

  if (isdigit((unsigned char)*s)) {
    zx80_int line_num = 0;
    const char *ns = parse_int(s, &line_num);
    if (!ns || line_num < 0 || line_num > 65535) {
      handle_error(vm, "BAD LINE");
      return -1;
    }
    ns = skip_ws(ns);
    if (*ns == '\0') {
      delete_line(vm, (uint16_t)line_num);
      return 0;
    }
    size_t len = strlen(ns);
    if (insert_line(vm, (uint16_t)line_num, ns, len) != 0) {
      handle_error(vm, "OUT OF MEMORY");
      return -1;
    }
    return 0;
  }

  uint16_t jump_line = 0xFFFF;
  const uint8_t *jump_ptr = NULL;
  int stop = 0;
  int res = exec_statement(vm, s, NULL, NULL, &jump_ptr, &jump_line, &stop);
  if (res < 0) {
    handle_error(vm, "SYNTAX ERROR");
    return -1;
  }
  if (jump_ptr) {
    return exec_program_from(vm, (uint8_t *)jump_ptr);
  }
  if (res == 1) {
    if (jump_line != 0xFFFF) {
      uint8_t *target = find_line(vm, jump_line, NULL);
      if (!target) {
        handle_error(vm, "LINE NOT FOUND");
        return -1;
      }
      return exec_program_from(vm, target);
    }
    return zx80_basic_run(vm);
  }
  return 0;
}
