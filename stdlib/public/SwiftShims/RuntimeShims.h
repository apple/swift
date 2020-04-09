//===--- RuntimeShims.h - Access to runtime facilities for the core -------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  Runtime functions and structures needed by the core stdlib are
//  declared here.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_STDLIB_SHIMS_RUNTIMESHIMS_H
#define SWIFT_STDLIB_SHIMS_RUNTIMESHIMS_H

#include "SwiftStddef.h"
#include "SwiftStdint.h"
#include "Visibility.h"

#ifdef __cplusplus
namespace swift { extern "C" {
#endif

/// Return an NSString to be used as the Mirror summary of the object
SWIFT_RUNTIME_STDLIB_API
void *_swift_objCMirrorSummary(const void * nsObject);

/// Call strtold_l with the C locale, swapping argument and return
/// types so we can operate on Float80.  Return NULL on overflow.
SWIFT_RUNTIME_STDLIB_API
const char *_swift_stdlib_strtold_clocale(const char *nptr, void *outResult);
/// Call strtod_l with the C locale, swapping argument and return
/// types so we can operate consistently on Float80.  Return NULL on
/// overflow.
SWIFT_RUNTIME_STDLIB_API
const char *_swift_stdlib_strtod_clocale(const char *nptr, double *outResult);
/// Call strtof_l with the C locale, swapping argument and return
/// types so we can operate consistently on Float80.  Return NULL on
/// overflow.
SWIFT_RUNTIME_STDLIB_API
const char *_swift_stdlib_strtof_clocale(const char *nptr, float *outResult);
/// Call strtof_l with the C locale, swapping argument and return
/// types so we can operate consistently on Float80.  Return NULL on
/// overflow.
SWIFT_RUNTIME_STDLIB_API
const char *_swift_stdlib_strtof16_clocale(const char *nptr, __fp16 *outResult);

SWIFT_RUNTIME_STDLIB_API
void _swift_stdlib_immortalize(void *obj);
  
SWIFT_RUNTIME_STDLIB_API
void _swift_stdlib_flockfile_stdout(void);
SWIFT_RUNTIME_STDLIB_API
void _swift_stdlib_funlockfile_stdout(void);

SWIFT_RUNTIME_STDLIB_API
int _swift_stdlib_putc_stderr(int C);

SWIFT_RUNTIME_STDLIB_API
__swift_size_t _swift_stdlib_getHardwareConcurrency(void);

SWIFT_RUNTIME_STDLIB_API
char *_swift_stdlib_demangle(const char *mangledName,
                             __swift_size_t mangledNameLength,
                             char *outputBuffer,
                             __swift_size_t *outputBufferSize,
                             __swift_uint32_t flags);

/// Manually allocated memory is at least 16-byte aligned in Swift.
///
/// When swift_slowAlloc is called with "default" alignment (alignMask ==
/// ~(size_t(0))), it will execute the "aligned allocation path" (AlignedAlloc)
/// using this value for the alignment.
///
/// This is done so users do not need to specify the allocation alignment when
/// manually deallocating memory via Unsafe[Raw][Buffer]Pointer. Any
/// user-specified alignment less than or equal to _swift_MinAllocationAlignment
/// results in a runtime request for "default" alignment. This guarantees that
/// manual allocation always uses an "aligned" runtime allocation. If an
/// allocation is "aligned" then it must be freed using an "aligned"
/// deallocation. The converse must also hold. Since manual Unsafe*Pointer
/// deallocation is always "aligned", the user never needs to specify alignment
/// during deallocation.
///
/// This value is inlined (and constant propagated) in user code. On Windows,
/// the Swift runtime and user binaries need to agree on this value.
#define _swift_MinAllocationAlignment (__swift_size_t) 16

#ifdef __cplusplus
}} // extern "C", namespace swift
#endif

#endif // SWIFT_STDLIB_SHIMS_RUNTIMESHIMS_H

