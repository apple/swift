//===--- GenPoly.cpp - Swift IR Generation for Polymorphism ---------------===//
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
//  This file implements IR generation for polymorphic operations in Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/Types.h"
#include "swift/AST/Decl.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILType.h"
#include "llvm/IR/DerivedTypes.h"

#include "Explosion.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "LoadableTypeInfo.h"
#include "TypeVisitor.h"
#include "GenTuple.h"
#include "GenPoly.h"
#include "GenType.h"

using namespace swift;
using namespace irgen;

static SILType applyPrimaryArchetypes(IRGenFunction &IGF,
                                      SILType type) {
  if (!type.hasTypeParameter()) {
    return type;
  }

  auto substType =
    IGF.IGM.getGenericEnvironment()->mapTypeIntoContext(type.getASTType())
      ->getCanonicalType();
  return SILType::getPrimitiveType(substType, type.getCategory());
}

/// Given a substituted explosion, re-emit it as an unsubstituted one.
///
/// For example, given an explosion which begins with the
/// representation of an (Int, Float), consume that and produce the
/// representation of an (Int, T).
///
/// The substitutions must carry origTy to substTy.
void irgen::reemitAsUnsubstituted(IRGenFunction &IGF,
                                  SILType expectedTy, SILType substTy,
                                  Explosion &in, Explosion &out) {
  expectedTy = applyPrimaryArchetypes(IGF, expectedTy);

  ExplosionSchema expectedSchema;
  cast<LoadableTypeInfo>(IGF.IGM.getTypeInfo(expectedTy))
    .getSchema(expectedSchema);

#ifndef NDEBUG
  auto &substTI = IGF.IGM.getTypeInfo(applyPrimaryArchetypes(IGF, substTy));
  assert(expectedSchema.size() ==
         cast<LoadableTypeInfo>(substTI).getExplosionSize());
#endif

  for (ExplosionSchema::Element &elt : expectedSchema) {
    llvm::Value *value = in.claimNext();
    assert(elt.isScalar());

    // The only type differences we expect here should be due to
    // substitution of class archetypes.
    if (value->getType() != elt.getScalarType()) {
      value = IGF.Builder.CreateBitCast(value, elt.getScalarType(),
                                        value->getName() + ".asUnsubstituted");
    }
    out.add(value);
  }
}
