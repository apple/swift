//===--- ConstantBuilder.h - IR generation for constant structs -*- C++ -*-===//
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
//  This file implements IR generation of constant packed LLVM structs.
//===----------------------------------------------------------------------===//

#include "swift/ABI/MetadataValues.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "clang/CodeGen/ConstantInitBuilder.h"

#include "Address.h"
#include "IRGenModule.h"
#include "IRGenFunction.h"

namespace clang {
class PointerAuthSchema;
}

namespace swift {
namespace irgen {
class PointerAuthEntity;

class ConstantAggregateBuilderBase;
class ConstantStructBuilder;
class ConstantArrayBuilder;
class ConstantInitBuilder;

struct ConstantInitBuilderTraits {
  using InitBuilder = ConstantInitBuilder;
  using AggregateBuilderBase = ConstantAggregateBuilderBase;
  using ArrayBuilder = ConstantArrayBuilder;
  using StructBuilder = ConstantStructBuilder;
};

/// A Swift customization of Clang's ConstantInitBuilder.
class ConstantInitBuilder
    : public clang::CodeGen::ConstantInitBuilderTemplateBase<
                                                    ConstantInitBuilderTraits> {
public:
  IRGenModule &IGM;
  ConstantInitBuilder(IRGenModule &IGM)
    : ConstantInitBuilderTemplateBase(IGM.getClangCGM()),
      IGM(IGM) {}
};

class ConstantAggregateBuilderBase
       : public clang::CodeGen::ConstantAggregateBuilderBase {
  using super = clang::CodeGen::ConstantAggregateBuilderBase;
protected:
  ConstantAggregateBuilderBase(ConstantInitBuilder &builder,
                               ConstantAggregateBuilderBase *parent)
    : super(builder, parent) {}

  ConstantInitBuilder &getBuilder() const {
    return static_cast<ConstantInitBuilder&>(Builder);
  }
  IRGenModule &IGM() const { return getBuilder().IGM; }

public:
  void addInt16(uint16_t value) {
    addInt(IGM().Int16Ty, value);
  }

  void addInt32(uint32_t value) {
    addInt(IGM().Int32Ty, value);
  }

  void addInt64(uint64_t value) { addInt(IGM().Int64Ty, value); }

  void addSize(Size size) { addInt(IGM().SizeTy, size.getValue()); }

  void addRelativeAddressOrNull(llvm::Constant *target) {
    if (target) {
      addRelativeAddress(target);
    } else {
      addInt(IGM().RelativeAddressTy, 0);
    }
  }

  void addRelativeAddress(llvm::Constant *target) {
    assert(!isa<llvm::ConstantPointerNull>(target));
    if (IGM().TargetInfo.OutputObjectFormat == llvm::Triple::Wasm) {
      // WebAssembly: hack: doesn't support PCrel data relocations
      add(llvm::ConstantExpr::getPtrToInt(target, IGM().RelativeAddressTy, false));
      return;
    }
    addRelativeOffset(IGM().RelativeAddressTy, target);
  }

  /// Add a tagged relative reference to the given address.  The direct
  /// target must be defined within the current image, but it might be
  /// a "GOT-equivalent", i.e. a pointer to an external object; if so,
  /// set the low bit of the offset to indicate that this is true.
  void addRelativeAddress(ConstantReference reference) {
    if (IGM().TargetInfo.OutputObjectFormat == llvm::Triple::Wasm) {
      // WebAssembly: hack: doesn't support PCrel data relocations
      // also, we should set the lowest bit, but I don't know how to do that
      // there's no GOT on WebAssembly anyways though


      llvm::Constant *offset = llvm::ConstantExpr::getPtrToInt(reference.getValue(), IGM().RelativeAddressTy, false);
      // borrowed from addTaggedRelativeOffset
      unsigned tag = unsigned(reference.isIndirect());
      if (tag) {
        offset = llvm::ConstantExpr::getAdd(offset, llvm::ConstantInt::get(IGM().RelativeAddressTy, tag));
      }
      add(offset);
      return;
    }
    addTaggedRelativeOffset(IGM().RelativeAddressTy,
                            reference.getValue(),
                            unsigned(reference.isIndirect()));
  }

  /// Add an indirect relative reference to the given address.
  /// The target must be a "GOT-equivalent", i.e. a pointer to an
  /// external object.
  void addIndirectRelativeAddress(ConstantReference reference) {
    if (IGM().TargetInfo.OutputObjectFormat == llvm::Triple::Wasm) {
      // WebAssembly: hack: doesn't support PCrel data relocations
      add(llvm::ConstantExpr::getPtrToInt(reference.getValue(), IGM().RelativeAddressTy, false));
      return;
    }
    assert(reference.isIndirect());
    addRelativeOffset(IGM().RelativeAddressTy,
                      reference.getValue());
  }

  Size getNextOffsetFromGlobal() const {
    return Size(super::getNextOffsetFromGlobal().getQuantity());
  }

  void addAlignmentPadding(Alignment align) {
    auto misalignment = getNextOffsetFromGlobal() % IGM().getPointerAlignment();
    if (misalignment != Size(0))
      add(llvm::ConstantAggregateZero::get(
            llvm::ArrayType::get(IGM().Int8Ty,
                                 align.getValue() - misalignment.getValue())));
  }

  using super::addSignedPointer;
  void addSignedPointer(llvm::Constant *pointer,
                        const clang::PointerAuthSchema &schema,
                        const PointerAuthEntity &entity);

  void addSignedPointer(llvm::Constant *pointer,
                        const clang::PointerAuthSchema &schema,
                        uint16_t otherDiscriminator);
};

class ConstantArrayBuilder
    : public clang::CodeGen::ConstantArrayBuilderTemplateBase<
                                                    ConstantInitBuilderTraits> {
private:
  llvm::Type *EltTy;

public:
  ConstantArrayBuilder(InitBuilder &builder,
                       AggregateBuilderBase *parent,
                       llvm::Type *eltTy)
    : ConstantArrayBuilderTemplateBase(builder, parent, eltTy), EltTy(eltTy) {}

  void addAlignmentPadding(Alignment align) {
    auto misalignment = getNextOffsetFromGlobal() % align;
    if (misalignment == Size(0))
      return;

    auto eltSize = IGM().DataLayout.getTypeStoreSize(EltTy);
    assert(misalignment.getValue() % eltSize == 0);

    for (unsigned i = 0, n = misalignment.getValue() / eltSize; i != n; ++i)
      add(llvm::Constant::getNullValue(EltTy));
  }
};

class ConstantStructBuilder
    : public clang::CodeGen::ConstantStructBuilderTemplateBase<
                                                    ConstantInitBuilderTraits> {
public:
  template <class... As>
  ConstantStructBuilder(As &&... args)
    : ConstantStructBuilderTemplateBase(std::forward<As>(args)...) {}
};

} // end namespace irgen
} // end namespace swift
