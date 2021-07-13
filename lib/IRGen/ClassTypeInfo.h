//===--- ClassTypeInfo.h - The layout info for class types. -----*- C++ -*-===//
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
//
//  This file contains layout information for class types.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_CLASSTYPEINFO_H
#define SWIFT_IRGEN_CLASSTYPEINFO_H

#include "ClassLayout.h"
#include "HeapTypeInfo.h"
#include "TypeLayout.h"

namespace swift {
namespace irgen {

/// Layout information for class types.
class ClassTypeInfo : public HeapTypeInfo<ClassTypeInfo> {
  ClassDecl *TheClass;

  // The resilient layout of the class, without making any assumptions
  // that violate resilience boundaries. This is used to allocate
  // and deallocate instances of the class, and to access fields.
  mutable Optional<ClassLayout> ResilientLayout;

  // A completely fragile layout, used for metadata emission.
  mutable Optional<ClassLayout> FragileLayout;

  /// Can we use swift reference-counting, or do we have to use
  /// objc_retain/release?
  const ReferenceCounting Refcount;

  ClassLayout generateLayout(IRGenModule &IGM, SILType classType,
                             bool forBackwardDeployment) const;

public:
  ClassTypeInfo(llvm::PointerType *irType, Size size, SpareBitVector spareBits,
                Alignment align, ClassDecl *theClass,
                ReferenceCounting refcount)
      : HeapTypeInfo(refcount, irType, size, std::move(spareBits), align),
        TheClass(theClass), Refcount(refcount) {}

  ReferenceCounting getReferenceCounting() const { return Refcount; }

  ClassDecl *getClass() const { return TheClass; }

  const ClassLayout &getClassLayout(IRGenModule &IGM, SILType type,
                                    bool forBackwardDeployment) const;

  StructLayout *createLayoutWithTailElems(IRGenModule &IGM, SILType classType,
                                          ArrayRef<SILType> tailTypes) const;
};

} // namespace irgen
} // namespace swift

#endif
