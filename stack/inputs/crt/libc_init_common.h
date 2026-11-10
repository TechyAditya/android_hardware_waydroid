// What bionic's crtbegin.c takes from libc/bionic/libc_init_common.h, which needs the rest of
// bionic's private headers
#pragma once

typedef void (*init_func_t)(int, char**, char**);
typedef void (*fini_func_t)(void);
typedef struct {
  init_func_t** preinit_array;
  init_func_t** init_array;
  fini_func_t** fini_array;
} structors_array_t;

int main(int, char**, char**);
void __libc_init(void* raw_args, void (*onexit)(void), int (*slingshot)(int, char**, char**),
                 structors_array_t const* const);
