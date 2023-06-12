//===--- FixedLayout.h - Types whose layout must be fixed -------*- C++ -*-===//
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
//  Defines types whose in-memory layout must be fixed, which therefore have
//  to be defined using C code.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BACKTRACING_FIXED_LAYOUT_H
#define SWIFT_BACKTRACING_FIXED_LAYOUT_H

#ifdef __cplusplus
namespace swift {
extern "C" {
#endif

#include <stdint.h>

struct x86_64_gprs {
  uint64_t _r[16];
  uint64_t rflags;
  uint16_t cs, fs, gs, _pad0;
  uint64_t rip;
  uint64_t valid;
};

struct i386_gprs {
  uint32_t _r[8];
  uint32_t eflags;
  uint16_t segreg[6];
  uint32_t eip;
  uint32_t valid;
};

struct arm64_gprs {
  uint64_t _x[32];
  uint64_t pc;
  uint64_t valid;
};

struct arm_gprs {
  uint32_t _r[16];
  uint32_t valid;
};

#ifdef __cpluspus
} // extern "C"
} // namespace swift
#endif

#endif // SWIFT_BACKTRACING_FIXED_LAYOUT_H
