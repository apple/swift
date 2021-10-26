//===--- SemanticARCOpts.h ------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_SEMANTICARC_SEMANTICARCOPTS_H
#define SWIFT_SILOPTIMIZER_SEMANTICARC_SEMANTICARCOPTS_H

#include <cstdint>
#include <type_traits>

namespace swift {
namespace semanticarc {

/// An enum used so that at the command line, we can override which transforms
/// we perform.
enum class ARCTransformKind : uint64_t {
  Invalid = 0,
  OwnedToGuaranteedPhi = 0x1,
  LoadCopyToLoadBorrowPeephole = 0x2,
  RedundantBorrowScopeElimPeephole = 0x4,
  // TODO: Split RedundantCopyValueElimPeephole into more granular categories
  // such as dead live range, guaranteed copy_value opt, etc.
  RedundantCopyValueElimPeephole = 0x8,
  LifetimeJoiningPeephole = 0x10,
  OwnershipConversionElimPeephole = 0x20,
  MoveValuePeepholes = 0x40,

  AllPeepholes = LoadCopyToLoadBorrowPeephole |
                 RedundantBorrowScopeElimPeephole |
                 RedundantCopyValueElimPeephole | LifetimeJoiningPeephole |
                 OwnershipConversionElimPeephole | MoveValuePeepholes,
  All = AllPeepholes | OwnedToGuaranteedPhi,
};

inline ARCTransformKind operator&(ARCTransformKind lhs, ARCTransformKind rhs) {
  using UnderlyingTy = std::underlying_type<ARCTransformKind>::type;
  return ARCTransformKind(UnderlyingTy(lhs) & UnderlyingTy(rhs));
}

} // namespace semanticarc
} // namespace swift

#endif
