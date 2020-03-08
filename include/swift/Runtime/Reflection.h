//===--- Reflection.h - Swift Language Reflection Support -------*- C++ -*-===//
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
// An implementation of value reflection based on type metadata.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/ExistentialContainer.h"
#include "swift/Runtime/Metadata.h"
#include <cstdlib>

namespace swift {
  
struct MirrorWitnessTable;
  
/// The layout of protocol<Mirror>.
struct Mirror {
  OpaqueExistentialContainer Header;
  const MirrorWitnessTable *MirrorWitness;
};

// Swift assumes Mirror is returned in memory. 
// Use MirrorReturn to guarantee that even on architectures 
// where Mirror would be returned in registers.
struct MirrorReturn {
  Mirror mirror;
  MirrorReturn(Mirror m) : mirror(m) { }
  operator Mirror() { return mirror; }
  ~MirrorReturn() { }
};

// We intentionally use a non-POD return type with these entry points to give
// them an indirect return ABI for compatibility with Swift.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"

/// func reflect<T>(x: T) -> Mirror
///
/// Produce a mirror for any value.  The runtime produces a mirror that
/// structurally reflects values of any type.
SWIFT_CC SWIFT_RUNTIME_EXPORT
MirrorReturn swift_reflectAny(OpaqueValue *value, const Metadata *T);

#pragma clang diagnostic pop

}
