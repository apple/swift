//===--- ProtocolDispatchStrategy.h - ---------------------------*- C++ -*-===//
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
// This header declares the ProtocolDispatchStrategy enum and some
// related operations.  It's split out because we would otherwise need
// to include MetadataValues.h in some relatively central headers.s
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_ABI_PROTOCOLDISPATCHSTRATEGY_H
#define SWIFT_ABI_PROTOCOLDISPATCHSTRATEGY_H

#include "swift/Basic/Unreachable.h"
#include <cstdint>

namespace swift {

/// Identifiers for protocol method dispatch strategies.
enum class ProtocolDispatchStrategy: uint8_t {
  /// Uses ObjC method dispatch.
  ///
  /// This must be 0 for ABI compatibility with Objective-C protocol_t records.
  ObjC = 0,
  
  /// Uses Swift protocol witness table dispatch.
  ///
  /// To invoke methods of this protocol, a pointer to a protocol witness table
  /// corresponding to the protocol conformance must be available.
  Swift = 1,
};

/// Does a protocol using the given strategy require a witness table?
inline bool protocolRequiresWitnessTable(ProtocolDispatchStrategy strategy) {
  switch (strategy) {
  case ProtocolDispatchStrategy::ObjC:
    return false;
  case ProtocolDispatchStrategy::Swift:
    return true;
  }

  swift_unreachable("Unhandled ProtocolDispatchStrategy in switch.");
}

}

#endif
