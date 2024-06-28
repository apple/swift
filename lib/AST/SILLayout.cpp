//===--- SILLayout.cpp - Defines SIL-level aggregate layouts --------------===//
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
// This file defines classes that describe the physical layout of nominal
// types in SIL, including structs, classes, and boxes. This is distinct from
// the AST-level layout for several reasons:
// - It avoids redundant work lowering the layout of aggregates from the AST.
// - It allows optimizations to manipulate the layout of aggregates without
//   requiring changes to the AST. For instance, optimizations can eliminate
//   dead fields from instances or turn invariant fields into global variables.
// - It allows for SIL-only aggregates to exist, such as boxes.
// - It improves the robustness of code in the face of resilience. A resilient
//   type can be modeled in SIL as not having a layout at all, preventing the
//   inappropriate use of fragile projection and injection operations on the
//   type.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/SILLayout.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Range.h"

using namespace swift;

static bool anyMutable(ArrayRef<SILField> Fields) {
  for (auto &field : Fields) {
    if (field.isMutable())
      return true;
  }
  return false;
}

#ifndef NDEBUG
/// Verify that the types of fields are valid within a given generic signature.
static void verifyFields(CanGenericSignature Sig, ArrayRef<SILField> Fields) {
  for (auto &field : Fields) {
    auto ty = field.getLoweredType();
    // Layouts should never refer to archetypes, since they represent an
    // abstract generic type layout.
    assert(!ty->hasArchetype()
           && "SILLayout field cannot have an archetype type");
    assert(!ty->hasTypeVariable()
           && "SILLayout cannot contain constraint system type variables");
    assert(!ty->hasPlaceholder() &&
           "SILLayout cannot contain constraint system type holes");
    if (!ty->hasTypeParameter())
      continue;
    field.getLoweredType().findIf([Sig](Type t) -> bool {
      if (auto gpt = t->getAs<GenericTypeParamType>()) {
        // Check that the generic param exists in the generic signature.
        assert(Sig && "generic param in nongeneric layout?");
        assert(std::find(Sig.getGenericParams().begin(),
                         Sig.getGenericParams().end(),
                         gpt->getCanonicalType()) != Sig.getGenericParams().end()
               && "generic param not declared in generic signature?!");
      }
      return false;
    });
  }
}
#endif

SILLayout::SILLayout(CanGenericSignature Sig,
                     ArrayRef<SILField> Fields,
                     bool CapturesGenericEnvironment)
  : GenericSigAndFlags(Sig,
                 getFlagsValue(anyMutable(Fields), CapturesGenericEnvironment)),
    NumFields(Fields.size())
{
#ifndef NDEBUG
  verifyFields(Sig, Fields);
#endif
  auto FieldsMem = getTrailingObjects<SILField>();
  for (unsigned i : indices(Fields)) {
    new (FieldsMem + i) SILField(Fields[i]);
  }
}

void SILLayout::Profile(llvm::FoldingSetNodeID &id,
                        CanGenericSignature Generics,
                        ArrayRef<SILField> Fields,
                        bool CapturesGenericEnvironment) {
  id.AddPointer(Generics.getPointer());
  id.AddBoolean(CapturesGenericEnvironment);
  for (auto &field : Fields) {
    id.AddPointer(field.getLoweredType().getPointer());
    id.AddBoolean(field.isMutable());
  }
}
