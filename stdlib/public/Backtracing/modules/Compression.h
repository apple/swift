//===--- Compression.h - C decls for compression libraries ------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Includes and definitions to allow us to use the compression libraries
// (zlib, zstd and liblzma) in the backtracing module.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BACKTRACING_COMPRESSION_H
#define SWIFT_BACKTRACING_COMPRESSION_H

#include <stdint.h>
#include <stdlib.h>

// Right now, we're soft linking to zlib/zstd/liblzma, so that users don't
// need it installed (but if they try to do something that requires it,
// they'll see an error message).
//
// As a result, we've grabbed copies of the relevant definitions here so
// that we don't need to install the -dev packages in order to build Swift.

#if SWIFT_BACKTRACE_STATIC_ZLIB
#include "zlib.h"
#else
// This is the version we took the z_stream structure from
#define ZLIB_VERSION "1.2.11"

#define Z_OK 0
#define Z_STREAM_END 1

#define Z_NO_FLUSH 0

typedef struct z_stream_s {
  uint8_t *next_in;
  unsigned avail_in;
  unsigned long total_in;

  uint8_t *next_out;
  unsigned avail_out;
  unsigned long total_out;

  const char *msg;
  struct internal_state *state;

  void (*zalloc)(void *, unsigned, unsigned);
  void (*zfree)(void *, void *);
  void *opaque;

  int data_type;

  unsigned long adler;
  unsigned long reserved;
} z_stream;

typedef z_stream *z_streamp;
#endif

#if SWIFT_BACKTRACE_STATIC_ZSTD
#include "zstd.h"
#else
typedef struct ZSTD_inBuffer_s {
  const void *src;
  size_t size;
  size_t pos;
} ZSTD_inBuffer;

typedef struct ZSTD_outBuffer_s {
  void *dst;
  size_t size;
  size_t pos;
} ZSTD_outBuffer;
#endif

#if SWIFT_BACKTRACE_STATIC_LIBLZMA
#include "lzma.h"
#else
typedef enum {
  LZMA_OK = 0,
  LZMA_STREAM_END = 1,
  LZMA_NO_CHECK = 2,
  LZMA_UNSUPPORTED_CHECK = 3,
  LZMA_GET_CHECK = 4,
  LZMA_MEM_ERROR = 5,
  LZMA_MEMLIMIT_ERROR = 6,
  LZMA_FORMAT_ERROR = 7,
  LZMA_OPTIONS_ERROR = 8,
  LZMA_DATA_ERROR = 9,
  LZMA_BUF_ERROR = 10,
  LZMA_PROG_ERROR = 11,
} lzma_ret;

typedef enum {
  LZMA_RUN = 0,
  LZMA_SYNC_FLUSH = 1,
  LZMA_FULL_FLUSH = 2,
  LZMA_FULL_BARRIER = 4,
  LZMA_FINISH = 3
} lzma_action;

typedef enum {
  LZMA_RESERVED_ENUM = 0,
} lzma_reserved_enum;

typedef struct {
  void *(*alloc)(void *, size_t, size_t);
  void (*free)(void *, void *);
  void *opaque;
} lzma_allocator;

typedef struct lzma_internal_s lzma_internal;

typedef struct {
  const uint8_t *next_in;
  size_t avail_in;
  uint64_t total_in;

  uint8_t *next_out;
  size_t avail_out;
  uint64_t total_out;

  const lzma_allocator *allocator;

  lzma_internal *internal;

  void *reserved_ptr1;
  void *reserved_ptr2;
  void *reserved_ptr3;
  void *reserved_ptr4;
  uint64_t reserved_int1;
  uint64_t reserved_int2;
  size_t reserved_int3;
  size_t reserved_int4;
  lzma_reserved_enum reserved_enum1;
  lzma_reserved_enum reserved_enum2;
} lzma_stream;

#define LZMA_STREAM_INIT {0}
#endif

#ifdef __cplusplus
namespace swift {
extern "C" {
#endif

// The Swift importer can't cope with complex macros; it will do inline
// functions, however.
static inline lzma_stream lzma_stream_init() {
  return (lzma_stream)LZMA_STREAM_INIT;
}
static inline z_stream zlib_stream_init() {
  return (z_stream){ 0 };
}

#ifdef __cplusplus
} // extern "C"
} // namespace swift
#endif

#endif // SWIFT_BACKTRACING_COMPRESSION_H
