// ZX80 BASIC minimal interpreter core (C)
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ZX80_BASIC_DEFAULT_RAM
#define ZX80_BASIC_DEFAULT_RAM 1024
#endif

#ifndef ZX80_BASIC_DEFAULT_ARRAY_MEM
#define ZX80_BASIC_DEFAULT_ARRAY_MEM 1024
#endif

#ifndef ZX80_BASIC_GOSUB_DEPTH
#define ZX80_BASIC_GOSUB_DEPTH 8
#endif

#ifndef ZX80_BASIC_FOR_DEPTH
#define ZX80_BASIC_FOR_DEPTH 8
#endif

#ifndef ZX80_BASIC_MAX_ARRAYS
#define ZX80_BASIC_MAX_ARRAYS 8
#endif

typedef int32_t zx80_int;

typedef struct {
  void (*write_char)(char c, void *user);
  int (*read_line)(char *buf, size_t max_len, void *user);
  int (*break_check)(void *user);
  void *user;
} zx80_io_t;

typedef struct {
  int var;
  zx80_int end;
  zx80_int step;
  const uint8_t *line_ptr;
} zx80_for_frame_t;

typedef struct {
  int var;
  int dims;
  zx80_int size1;
  zx80_int size2;
  size_t offset;
  size_t bytes;
} zx80_array_t;

typedef struct {
  uint8_t *ram;
  size_t ram_size;
  size_t prog_end;
  zx80_int vars[26];
  const uint8_t *gosub_stack[ZX80_BASIC_GOSUB_DEPTH];
  int gosub_sp;
  zx80_for_frame_t for_stack[ZX80_BASIC_FOR_DEPTH];
  int for_sp;
  const uint8_t *cont_ptr;
  uint32_t rand_state;
  zx80_array_t arrays[ZX80_BASIC_MAX_ARRAYS];
  int array_count;
  uint8_t *array_mem;
  size_t array_mem_size;
  size_t array_mem_used;
  zx80_io_t io;
} zx80_basic_t;

void zx80_basic_init(zx80_basic_t *vm, uint8_t *ram, size_t ram_size,
                     zx80_io_t io);
void zx80_basic_init_default(zx80_basic_t *vm, zx80_io_t io);
void zx80_basic_reset(zx80_basic_t *vm);

int zx80_basic_handle_line(zx80_basic_t *vm, const char *line);
int zx80_basic_run(zx80_basic_t *vm);
void zx80_basic_list(zx80_basic_t *vm);

#ifdef __cplusplus
}
#endif
