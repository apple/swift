//===--- DefineDiagnosticMacros.h - Shared Diagnostic Macros ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file defines macros shared across diagnostic definition files.
//
//===----------------------------------------------------------------------===//

// Define macros
#ifdef DEFINE_DIAGNOSTIC_MACROS

#if !(defined(DIAG) || (defined(ERROR) && defined(WARNING) && defined(NOTE) && \
                        defined(REMARK)))
#error Must define either DIAG or the set {ERROR,WARNING,NOTE,REMARK}
#endif

#ifndef ERROR
#define ERROR(ID, Options, Text, Signature)                                    \
  DIAG(ERROR, ID, Options, Text, Signature)
#endif

#ifndef WARNING
#define WARNING(ID, Options, Text, Signature)                                  \
  DIAG(WARNING, ID, Options, Text, Signature)
#endif

#ifndef NOTE
#define NOTE(ID, Options, Text, Signature)                                     \
  DIAG(NOTE, ID, Options, Text, Signature)
#endif

#ifndef LINT
#define LINT(ID, Options, Text, Signature)                                     \
  DIAG(LINT, ID, Options, Text, Signature)
#endif

#ifndef REMARK
#define REMARK(ID, Options, Text, Signature)                                   \
  DIAG(REMARK, ID, Options, Text, Signature)
#endif

#ifndef FIXIT
#define FIXIT(ID, Text, Signature)
#endif

#undef DEFINE_DIAGNOSTIC_MACROS
#endif

// Undefine macros
#ifdef UNDEFINE_DIAGNOSTIC_MACROS

#ifndef DIAG_NO_UNDEF

#if defined(DIAG)
#undef DIAG
#endif

#undef REMARK
#undef NOTE
#undef LINT
#undef WARNING
#undef ERROR
#undef FIXIT

#endif

#undef UNDEFINE_DIAGNOSTIC_MACROS
#endif
