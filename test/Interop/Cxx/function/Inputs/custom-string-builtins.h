#pragma once

#include <stddef.h>

#define __attribute_pure__ __attribute__((__pure__))

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
    #define TEST_CONST_RETURN  const
#else
    #define TEST_CONST_RETURN
#endif


void* _Nonnull memcpy(void* _Nonnull, const void* _Nonnull, size_t);

void* _Nonnull memcpy42(void* _Nonnull, const void* _Nonnull, size_t);

void TEST_CONST_RETURN* _Nullable memchr(const void* _Nonnull __s, int __ch, size_t __n) __attribute_pure__;

void* _Nonnull memmove(void* _Nonnull __dst, const void* _Nonnull __src, size_t __n);

void* _Nonnull memset(void* _Nonnull __dst, int __ch, size_t __n);

char TEST_CONST_RETURN* strrchr(const char* __s, int __ch) __attribute_pure__;

char* _Nonnull strcpy(char* _Nonnull __dst, const char* _Nonnull __src);
char* _Nonnull strcat(char* _Nonnull __dst, const char* _Nonnull __src);

#ifdef __cplusplus
}
#endif
