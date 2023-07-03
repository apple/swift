//===--- MoveOnlyTypeUtils.cpp --------------------------------------------===//
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

#include "MoveOnlyTypeUtils.h"

using namespace swift;
using namespace swift::siloptimizer;

static StructDecl *getFullyReferenceableStruct(SILType ktypeTy) {
  auto structDecl = ktypeTy.getStructOrBoundGenericStruct();
  if (!structDecl || structDecl->hasUnreferenceableStorage())
    return nullptr;
  return structDecl;
}

llvm::Optional<std::pair<TypeOffsetSizePair, SILType>>
TypeOffsetSizePair::walkOneLevelTowardsChild(
    TypeOffsetSizePair ancestorOffsetSize, SILType ancestorType,
    SILFunction *fn) const {
  assert(ancestorOffsetSize.size >= size &&
         "Too large to be a child of ancestorType");
  assert((ancestorOffsetSize.startOffset <= startOffset &&
          startOffset <
              (ancestorOffsetSize.startOffset + ancestorOffsetSize.size)) &&
         "Not within the offset range of ancestor");

  if (auto tupleType = ancestorType.getAs<TupleType>()) {
    // Before we do anything, see if we have a single element tuple. If we do,
    // just return that.
    if (tupleType->getNumElements() == 1) {
      return {{ancestorOffsetSize, ancestorType.getTupleElementType(0)}};
    }

    assert(ancestorOffsetSize.size > size &&
           "Too large to be a child of ancestorType");

    unsigned childOffset = ancestorOffsetSize.startOffset;

    for (auto index : indices(tupleType->getElementTypes())) {
      SILType newType = ancestorType.getTupleElementType(index);
      unsigned newSize = TypeSubElementCount(newType, fn);

      // childOffset + size(tupleChild) is the offset of the next tuple
      // element. If our target offset is less than that, then we know that
      // the target type must be a descendent of this tuple element type.
      if (childOffset + newSize > startOffset) {
        return {{{childOffset, newSize}, newType}};
      }

      // Otherwise, add the new size of this field to iterOffset so we visit
      // our sibling type next.
      childOffset += newSize;
    }

    // At this point, we know that our type is not a subtype of this
    // type. Some sort of logic error occurred.
    llvm_unreachable("Not a child of this type?!");
  }

  if (auto *structDecl = getFullyReferenceableStruct(ancestorType)) {
    // Before we do anything, see if we have a single element struct. If we
    // do, just return that.
    auto storedProperties = structDecl->getStoredProperties();
    if (storedProperties.size() == 1) {
      return {{ancestorOffsetSize,
               ancestorType.getFieldType(storedProperties[0], fn)}};
    }

    assert(ancestorOffsetSize.size > size &&
           "Too large to be a child of ancestorType");

    unsigned childOffset = ancestorOffsetSize.startOffset;
    for (auto *fieldDecl : storedProperties) {
      SILType newType = ancestorType.getFieldType(fieldDecl, fn);
      unsigned newSize = TypeSubElementCount(newType, fn);

      // iterOffset + size(tupleChild) is the offset of the next tuple
      // element. If our target offset is less than that, then we know that
      // the target type must be a child of this tuple element type.
      if (childOffset + newSize > startOffset) {
        return {{{childOffset, newSize}, newType}};
      }

      // Otherwise, add the new size of this field to iterOffset so we visit
      // our sibling type next.
      childOffset += newSize;
    }

    // At this point, we know that our type is not a subtype of this
    // type. Some sort of logic error occurred.
    llvm_unreachable("Not a child of this type?!");
  }

  if (auto *enumDecl = ancestorType.getEnumOrBoundGenericEnum()) {
    llvm_unreachable("Cannot find child type of enum!\n");
  }

  llvm_unreachable("Hit a leaf type?! Should have handled it earlier");
}

/// Given an ancestor offset \p ancestorOffset and a type called \p
/// ancestorType, walk one level towards this current type inserting on value,
/// the relevant projection.
llvm::Optional<std::pair<TypeOffsetSizePair, SILValue>>
TypeOffsetSizePair::walkOneLevelTowardsChild(
    SILBuilderWithScope &builder, SILLocation loc,
    TypeOffsetSizePair ancestorOffsetSize, SILValue ancestorValue) const {
  auto *fn = ancestorValue->getFunction();
  SILType ancestorType = ancestorValue->getType();

  assert(ancestorOffsetSize.size >= size &&
         "Too large to be a child of ancestorType");
  assert((ancestorOffsetSize.startOffset <= startOffset &&
          startOffset <
              (ancestorOffsetSize.startOffset + ancestorOffsetSize.size)) &&
         "Not within the offset range of ancestor");
  if (auto tupleType = ancestorType.getAs<TupleType>()) {
    // Before we do anything, see if we have a single element tuple. If we do,
    // just return that.
    if (tupleType->getNumElements() == 1) {
      auto *newValue = builder.createTupleExtract(loc, ancestorValue, 0);
      return {{ancestorOffsetSize, newValue}};
    }

    assert(ancestorOffsetSize.size > size &&
           "Too large to be a child of ancestorType");

    unsigned childOffset = ancestorOffsetSize.startOffset;

    for (auto index : indices(tupleType->getElementTypes())) {
      SILType newType = ancestorType.getTupleElementType(index);
      unsigned newSize = TypeSubElementCount(newType, fn);

      // childOffset + size(tupleChild) is the offset of the next tuple
      // element. If our target offset is less than that, then we know that
      // the target type must be a descendent of this tuple element type.
      if (childOffset + newSize > startOffset) {
        auto *newValue = builder.createTupleExtract(loc, ancestorValue, index);
        return {{{childOffset, newSize}, newValue}};
      }

      // Otherwise, add the new size of this field to iterOffset so we visit
      // our sibling type next.
      childOffset += newSize;
    }

    // At this point, we know that our type is not a subtype of this
    // type. Some sort of logic error occurred.
    llvm_unreachable("Not a child of this type?!");
  }

  if (auto *structDecl = getFullyReferenceableStruct(ancestorType)) {
    // Before we do anything, see if we have a single element struct. If we
    // do, just return that.
    auto storedProperties = structDecl->getStoredProperties();
    if (storedProperties.size() == 1) {
      auto *newValue =
          builder.createStructExtract(loc, ancestorValue, storedProperties[0]);
      return {{ancestorOffsetSize, newValue}};
    }

    assert(ancestorOffsetSize.size > size &&
           "Too large to be a child of ancestorType");

    unsigned childOffset = ancestorOffsetSize.startOffset;
    for (auto *fieldDecl : structDecl->getStoredProperties()) {
      SILType newType = ancestorType.getFieldType(fieldDecl, fn);
      unsigned newSize = TypeSubElementCount(newType, fn);

      // iterOffset + size(tupleChild) is the offset of the next tuple
      // element. If our target offset is less than that, then we know that
      // the target type must be a child of this tuple element type.
      if (childOffset + newSize > startOffset) {
        auto *newValue =
            builder.createStructExtract(loc, ancestorValue, fieldDecl);
        return {{{childOffset, newSize}, newValue}};
      }

      // Otherwise, add the new size of this field to iterOffset so we visit
      // our sibling type next.
      childOffset += newSize;
    }

    // At this point, we know that our type is not a subtype of this
    // type. Some sort of logic error occurred.
    llvm_unreachable("Not a child of this type?!");
  }

  if (auto *enumDecl = ancestorType.getEnumOrBoundGenericEnum()) {
    llvm_unreachable("Cannot find child type of enum!\n");
  }

  llvm_unreachable("Hit a leaf type?! Should have handled it earlier");
}

/// Given an ancestor offset \p ancestorOffset and a type called \p
/// ancestorType, walk one level towards this current type which is assumed to
/// be a child type of \p ancestorType.
void TypeOffsetSizePair::constructPathString(
    SILType targetType, TypeOffsetSizePair ancestorOffsetSize,
    SILType ancestorType, SILFunction *fn, llvm::raw_ostream &os) const {
  TypeOffsetSizePair iterPair = ancestorOffsetSize;
  SILType iterType = ancestorType;

  do {
    assert(iterPair.size >= size && "Too large to be a child of iterType");
    assert((iterPair.startOffset <= startOffset &&
            startOffset < (iterPair.startOffset + iterPair.size)) &&
           "Not within the offset range of ancestor");

    if (auto tupleType = iterType.getAs<TupleType>()) {
      // Before we do anything, see if we have a single element tuple. If we
      // do, just return that.
      if (tupleType->getNumElements() == 1) {
        os << ".0";
        iterType = iterType.getTupleElementType(0);
        continue;
      }

      assert(iterPair.size > size && "Too large to be a child of iterType");

      unsigned childOffset = iterPair.startOffset;

      bool foundValue = false;
      for (auto index : indices(tupleType->getElementTypes())) {
        SILType newType = iterType.getTupleElementType(index);
        unsigned newSize = TypeSubElementCount(newType, fn);

        // childOffset + size(tupleChild) is the offset of the next tuple
        // element. If our target offset is less than that, then we know that
        // the target type must be a descendent of this tuple element type.
        if (childOffset + newSize > startOffset) {
          os << '.';
          os << index;
          iterPair = {childOffset, newSize};
          iterType = newType;
          foundValue = true;
          break;
        }

        // Otherwise, add the new size of this field to iterOffset so we visit
        // our sibling type next.
        childOffset += newSize;
      }

      if (foundValue)
        continue;

      // At this point, we know that our type is not a subtype of this
      // type. Some sort of logic error occurred.
      llvm_unreachable("Not a child of this type?!");
    }

    if (auto *structDecl = getFullyReferenceableStruct(iterType)) {
      // Before we do anything, see if we have a single element struct. If we
      // do, just return that.
      auto storedProperties = structDecl->getStoredProperties();
      if (storedProperties.size() == 1) {
        os << '.';
        os << storedProperties[0]->getBaseName().userFacingName();
        iterType = iterType.getFieldType(storedProperties[0], fn);
        continue;
      }

      assert(iterPair.size > size && "Too large to be a child of iterType");

      unsigned childOffset = iterPair.startOffset;
      bool foundValue = false;
      for (auto *fieldDecl : storedProperties) {
        SILType newType = iterType.getFieldType(fieldDecl, fn);
        unsigned newSize = TypeSubElementCount(newType, fn);

        // iterOffset + size(tupleChild) is the offset of the next tuple
        // element. If our target offset is less than that, then we know that
        // the target type must be a child of this tuple element type.
        if (childOffset + newSize > startOffset) {
          os << '.';
          os << fieldDecl->getBaseName().userFacingName();
          iterPair = {childOffset, newSize};
          iterType = newType;
          foundValue = true;
          break;
        }

        // Otherwise, add the new size of this field to iterOffset so we visit
        // our sibling type next.
        childOffset += newSize;
      }

      if (foundValue)
        continue;

      // At this point, we know that our type is not a subtype of this
      // type. Some sort of logic error occurred.
      llvm_unreachable("Not a child of this type?!");
    }

    if (auto *enumDecl = iterType.getEnumOrBoundGenericEnum()) {
      llvm_unreachable("Cannot find child type of enum!\n");
    }

    llvm_unreachable("Hit a leaf type?! Should have handled it earlier");
  } while (iterType != targetType);
}
