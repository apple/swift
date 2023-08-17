//===--- Nullability.h ----------------------------------------------------===//
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

#ifndef SWIFT_BASIC_NULLABILITY_H
#define SWIFT_BASIC_NULLABILITY_H

// TODO: These macro definitions are duplicated in Visibility.h. Move
// them to a single file if we find a location that both Visibility.h and
// BridgedSwiftObject.h can import.
#if __has_feature(nullability)
// Provide macros to temporarily suppress warning about the use of
// _Nullable and _Nonnull.
#define SWIFT_BEGIN_NULLABILITY_ANNOTATIONS                                    \
  _Pragma("clang diagnostic push")                                             \
      _Pragma("clang diagnostic ignored \"-Wnullability-extension\"")
#define SWIFT_END_NULLABILITY_ANNOTATIONS _Pragma("clang diagnostic pop")

#define SWIFT_BEGIN_ASSUME_NONNULL _Pragma("clang assume_nonnull begin")
#define SWIFT_END_ASSUME_NONNULL _Pragma("clang assume_nonnull end")
#else
// #define _Nullable and _Nonnull to nothing if we're not being built
// with a compiler that supports them.
#define _Nullable
#define _Nonnull
#define _Null_unspecified
#define SWIFT_BEGIN_NULLABILITY_ANNOTATIONS
#define SWIFT_END_NULLABILITY_ANNOTATIONS
#define SWIFT_BEGIN_ASSUME_NONNULL
#define SWIFT_END_ASSUME_NONNULL
#endif

#endif
