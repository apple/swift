//===--- ContextualTypeInfo.h - Contextual Type Info ------------*- C++ -*-===//
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
// This file provides the \c ContextualTypeInfo class.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SEMA_CONTEXTUAL_TYPE_INFO_H
#define SWIFT_SEMA_CONTEXTUAL_TYPE_INFO_H

#include "swift/AST/TypeLoc.h"
#include "swift/Sema/ConstraintLocator.h"

namespace swift {
namespace constraints {

/// Describes contextual type information about a particular element
/// (expression, statement etc.) within a constraint system.
struct ContextualTypeInfo {
  TypeLoc typeLoc;
  ContextualTypePurpose purpose;

  /// The locator for the contextual type conversion constraint, or
  /// \c nullptr to use the default locator which is anchored directly on
  /// the expression.
  ConstraintLocator *locator;

  ContextualTypeInfo()
      : typeLoc(TypeLoc()), purpose(CTP_Unused), locator(nullptr) {}

  ContextualTypeInfo(Type contextualTy, ContextualTypePurpose purpose,
                     ConstraintLocator *locator = nullptr)
      : typeLoc(TypeLoc::withoutLoc(contextualTy)), purpose(purpose),
        locator(locator) {}

  ContextualTypeInfo(TypeLoc typeLoc, ContextualTypePurpose purpose,
                     ConstraintLocator *locator = nullptr)
      : typeLoc(typeLoc), purpose(purpose), locator(locator) {}

  Type getType() const { return typeLoc.getType(); }
};

} // end namespace constraints
} // end namespace swift

#endif // SWIFT_SEMA_CONTEXTUAL_TYPE_INFO_H
