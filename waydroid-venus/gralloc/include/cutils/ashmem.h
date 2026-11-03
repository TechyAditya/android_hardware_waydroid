#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// From libcutils, which the NDK does not ship headers for.
int ashmem_create_region(const char *name, size_t size);

#ifdef __cplusplus
}
#endif
