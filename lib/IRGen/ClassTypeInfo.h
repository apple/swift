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

#include "swift/ClangImporter/ClangImporterRequests.h"

namespace swift {
namespace irgen {

/// Layout information for class types.
class ClassTypeInfo : public HeapTypeInfo<ClassTypeInfo> {
  ClassDecl *TheClass;
  mutable llvm::StructType *classLayoutType;

  // The resilient layout of the class, without making any assumptions
  // that violate resilience boundaries. This is used to allocate
  // and deallocate instances of the class, and to access fields.
  mutable llvm::Optional<ClassLayout> ResilientLayout;

  // A completely fragile layout, used for metadata emission.
  mutable llvm::Optional<ClassLayout> FragileLayout;

  /// Can we use swift reference-counting, or do we have to use
  /// objc_retain/release?
  const ReferenceCounting Refcount;

  ClassLayout generateLayout(IRGenModule &IGM, SILType classType,
                             bool forBackwardDeployment) const;

public:
  ClassTypeInfo(llvm::PointerType *irType, Size size, SpareBitVector spareBits,
                Alignment align, ClassDecl *theClass,
                ReferenceCounting refcount, llvm::StructType *classLayoutType)
      : HeapTypeInfo(refcount, irType, size, std::move(spareBits), align),
        TheClass(theClass), classLayoutType(classLayoutType),
        Refcount(refcount) {}

  ReferenceCounting getReferenceCounting() const { return Refcount; }

  ClassDecl *getClass() const { return TheClass; }

  llvm::Type *getClassLayoutType() const { return classLayoutType; }

  const ClassLayout &getClassLayout(IRGenModule &IGM, SILType type,
                                    bool forBackwardDeployment) const;

  StructLayout *createLayoutWithTailElems(IRGenModule &IGM, SILType classType,
                                          ArrayRef<SILType> tailTypes) const;

  void emitScalarRelease(IRGenFunction &IGF, llvm::Value *value,
                         Atomicity atomicity) const override {
    if (getReferenceCounting() == ReferenceCounting::Custom) {
      auto releaseFn =
          evaluateOrDefault(
              getClass()->getASTContext().evaluator,
              CustomRefCountingOperation(
                  {getClass(), CustomRefCountingOperationKind::release}),
              {})
              .operation;
      IGF.emitForeignReferenceTypeLifetimeOperation(releaseFn, value);
      return;
    }

    HeapTypeInfo::emitScalarRelease(IGF, value, atomicity);
  }

  void emitScalarRetain(IRGenFunction &IGF, llvm::Value *value,
                        Atomicity atomicity) const override {
    if (getReferenceCounting() == ReferenceCounting::Custom) {
      auto retainFn =
          evaluateOrDefault(
              getClass()->getASTContext().evaluator,
              CustomRefCountingOperation(
                  {getClass(), CustomRefCountingOperationKind::retain}),
              {})
              .operation;
      IGF.emitForeignReferenceTypeLifetimeOperation(retainFn, value);
      return;
    }

    HeapTypeInfo::emitScalarRetain(IGF, value, atomicity);
  }

  // Implement the primary retain/release operations of ReferenceTypeInfo
  // using basic reference counting.
  void strongRetain(IRGenFunction &IGF, Explosion &e,
                    Atomicity atomicity) const override {
    if (getReferenceCounting() == ReferenceCounting::Custom) {
      llvm::Value *value = e.claimNext();
      auto retainFn =
          evaluateOrDefault(
              getClass()->getASTContext().evaluator,
              CustomRefCountingOperation(
                  {getClass(), CustomRefCountingOperationKind::retain}),
              {})
              .operation;
      IGF.emitForeignReferenceTypeLifetimeOperation(retainFn, value);
      return;
    }

    HeapTypeInfo::strongRetain(IGF, e, atomicity);
  }

  void strongRelease(IRGenFunction &IGF, Explosion &e,
                     Atomicity atomicity) const override {
    if (getReferenceCounting() == ReferenceCounting::Custom) {
      llvm::Value *value = e.claimNext();
      auto releaseFn =
          evaluateOrDefault(
              getClass()->getASTContext().evaluator,
              CustomRefCountingOperation(
                  {getClass(), CustomRefCountingOperationKind::release}),
              {})
              .operation;
      IGF.emitForeignReferenceTypeLifetimeOperation(releaseFn, value);
      return;
    }

    HeapTypeInfo::strongRelease(IGF, e, atomicity);
  }
};

} // namespace irgen
} // namespace swift

#endif
