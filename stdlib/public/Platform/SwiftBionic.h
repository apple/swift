//===--- SwiftBionic.h ----------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BIONIC_MODULE
#define SWIFT_BIONIC_MODULE

#include <complex.h>
#include <ctype.h>
#include <errno.h>
#include <fenv.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <malloc.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#ifdef __cplusplus
// The Android r26 NDK contains an old libc++ modulemap that requires C++23
// for 'stdatomic', which can't be imported unless we're using C++23. Thus,
// import stdatomic from the NDK directly, bypassing the stdatomic from the libc++.
#pragma clang module import _stdatomic
#else
#include <stdatomic.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdio_ext.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <threads.h>
#include <uchar.h>
#include <wchar.h>

#endif // SWIFT_BIONIC_MODULE
