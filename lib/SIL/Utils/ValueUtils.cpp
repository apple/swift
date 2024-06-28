//===--- ValueUtils.cpp ---------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/SIL/ValueUtils.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/STLExtras.h"
#include "swift/SIL/SILFunction.h"

using namespace swift;

ValueOwnershipKind swift::getSILValueOwnership(ArrayRef<SILValue> values,
                                               SILType ty) {
  auto range = makeTransformRange(values, [](SILValue v) {
    assert(v->getType().isObject());
    return v->getOwnershipKind();
  });

  auto mergedOwnership = ValueOwnershipKind::merge(range);

  // If we have a move only type, and the merged ownership is none, we return
  // Owned ownership kind.
  if (ty && ty.isMoveOnly() && mergedOwnership == OwnershipKind::None) {
    return OwnershipKind::Owned;
  }
  return mergedOwnership;
}
