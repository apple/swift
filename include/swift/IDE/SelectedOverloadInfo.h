//===--- SelectedOverloadInfo.h -------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IDE_SWIFTCOMPLETIONINFO_H
#define SWIFT_IDE_SWIFTCOMPLETIONINFO_H

#include "swift/AST/Decl.h"
#include "swift/Sema/ConstraintSystem.h"

namespace swift {
namespace ide {

using namespace swift::constraints;

/// Information that \c getSelectedOverloadInfo gathered about a
/// \c SelectedOverload.
struct SelectedOverloadInfo {
  /// The function that is being called or the value that is being accessed.
  ConcreteDeclRef ValueRef;
  /// For a function, type of the called function itself (not its result type),
  /// for an arbitrary value the type of that value.
  Type ValueTy;
  /// The type on which the overload is being accessed. \c null if it does not
  /// have a base type, e.g. for a free function.
  Type BaseTy;

  ValueDecl *getValue() const { return ValueRef.getDecl(); }
};

/// Extract additional information about the overload that is being called by
/// \p CalleeLocator.
SelectedOverloadInfo getSelectedOverloadInfo(const Solution &S,
                                             ConstraintLocator *CalleeLocator);

} // end namespace ide
} // end namespace swift

#endif // SWIFT_IDE_SWIFTCOMPLETIONINFO_H
