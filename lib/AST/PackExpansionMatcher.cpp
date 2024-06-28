//===--- PackExpansionMatcher.cpp - Matching pack expansions --------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Utilities for structural matching of sequences of types containing pack
// expansions.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/PackExpansionMatcher.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Type.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Assertions.h"
#include "llvm/ADT/SmallVector.h"
#include <algorithm>

using namespace swift;

template <>
Identifier TypeListPackMatcher<TupleTypeElt>::getElementLabel(
    const TupleTypeElt &elt) const {
  return elt.getName();
}

template <>
Type TypeListPackMatcher<TupleTypeElt>::getElementType(
    const TupleTypeElt &elt) const {
  return elt.getType();
}

template <>
ParameterTypeFlags TypeListPackMatcher<TupleTypeElt>::getElementFlags(
    const TupleTypeElt &elt) const {
  return ParameterTypeFlags();
}

template <>
Identifier TypeListPackMatcher<AnyFunctionType::Param>::getElementLabel(
    const AnyFunctionType::Param &elt) const {
  return elt.getLabel();
}

template <>
Type TypeListPackMatcher<AnyFunctionType::Param>::getElementType(
    const AnyFunctionType::Param &elt) const {
  return elt.getPlainType();
}

template <>
ParameterTypeFlags TypeListPackMatcher<AnyFunctionType::Param>::getElementFlags(
    const AnyFunctionType::Param &elt) const {
  return elt.getParameterFlags();
}

template <>
Identifier TypeListPackMatcher<Type>::getElementLabel(const Type &elt) const {
  return Identifier();
}

template <>
Type TypeListPackMatcher<Type>::getElementType(const Type &elt) const {
  return elt;
}

template <>
ParameterTypeFlags
TypeListPackMatcher<Type>::getElementFlags(const Type &elt) const {
  return ParameterTypeFlags();
}
