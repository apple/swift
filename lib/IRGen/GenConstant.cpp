//===--- GenConstant.cpp - Swift IR Generation For Constants --------------===//
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
//  This file implements IR generation for constant values.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Constants.h"

#include "BitPatternReader.h"
#include "Explosion.h"
#include "GenConstant.h"
#include "GenEnum.h"
#include "GenIntegerLiteral.h"
#include "GenStruct.h"
#include "GenTuple.h"
#include "TypeInfo.h"
#include "StructLayout.h"
#include "Callee.h"
#include "ConstantBuilder.h"
#include "swift/Basic/Range.h"
#include "swift/SIL/SILModule.h"
#include "llvm/Support/BLAKE3.h"

using namespace swift;
using namespace irgen;

llvm::Constant *irgen::emitConstantInt(IRGenModule &IGM,
                                       IntegerLiteralInst *ILI) {
  BuiltinIntegerWidth width
    = ILI->getType().castTo<AnyBuiltinIntegerType>()->getWidth();

  // Handle arbitrary-precision integers.
  if (width.isArbitraryWidth()) {
    auto pair = emitConstantIntegerLiteral(IGM, ILI);
    auto type = IGM.getIntegerLiteralTy();
    return llvm::ConstantStruct::get(type, { pair.Data, pair.Flags });
  }

  APInt value = ILI->getValue();

  // The value may need truncation if its type had an abstract size.
  if (width.isPointerWidth()) {
    unsigned pointerWidth = IGM.getPointerSize().getValueInBits();
    assert(pointerWidth <= value.getBitWidth()
           && "lost precision at AST/SIL level?!");
    if (pointerWidth < value.getBitWidth())
      value = value.trunc(pointerWidth);
  } else {
    assert(width.isFixedWidth() && "impossible width value");
  }

  return llvm::ConstantInt::get(IGM.getLLVMContext(), value);
}

llvm::Constant *irgen::emitConstantZero(IRGenModule &IGM, BuiltinInst *BI) {
  assert(IGM.getSILModule().getBuiltinInfo(BI->getName()).ID ==
         BuiltinValueKind::ZeroInitializer);

  auto helper = [&](CanType astType) -> llvm::Constant * {
    if (auto type = astType->getAs<BuiltinIntegerType>()) {
      APInt zero(type->getWidth().getLeastWidth(), 0);
      return llvm::ConstantInt::get(IGM.getLLVMContext(), zero);
    }

    if (auto type = astType->getAs<BuiltinFloatType>()) {
      const llvm::fltSemantics *sema = nullptr;
      switch (type->getFPKind()) {
      case BuiltinFloatType::IEEE16: sema = &APFloat::IEEEhalf(); break;
      case BuiltinFloatType::IEEE32: sema = &APFloat::IEEEsingle(); break;
      case BuiltinFloatType::IEEE64: sema = &APFloat::IEEEdouble(); break;
      case BuiltinFloatType::IEEE80: sema = &APFloat::x87DoubleExtended(); break;
      case BuiltinFloatType::IEEE128: sema = &APFloat::IEEEquad(); break;
      case BuiltinFloatType::PPC128: sema = &APFloat::PPCDoubleDouble(); break;
      }
      auto zero = APFloat::getZero(*sema);
      return llvm::ConstantFP::get(IGM.getLLVMContext(), zero);
    }

    llvm_unreachable("SIL allowed an unknown type?");
  };

  if (auto vector = BI->getType().getAs<BuiltinVectorType>()) {
    auto zero = helper(vector.getElementType());
    auto count = llvm::ElementCount::getFixed(vector->getNumElements());
    return llvm::ConstantVector::getSplat(count, zero);
  }

  return helper(BI->getType().getASTType());
}

llvm::Constant *irgen::emitConstantFP(IRGenModule &IGM, FloatLiteralInst *FLI) {
  return llvm::ConstantFP::get(IGM.getLLVMContext(), FLI->getValue());
}

llvm::Constant *irgen::emitAddrOfConstantString(IRGenModule &IGM,
                                                StringLiteralInst *SLI) {
  switch (SLI->getEncoding()) {
  case StringLiteralInst::Encoding::Bytes:
  case StringLiteralInst::Encoding::UTF8:
    return IGM.getAddrOfGlobalString(SLI->getValue());

  case StringLiteralInst::Encoding::ObjCSelector:
    llvm_unreachable("cannot get the address of an Objective-C selector");
  }
  llvm_unreachable("bad string encoding");
}

namespace {

/// Fill in the missing values for padding.
void insertPadding(SmallVectorImpl<Explosion> &elements,
                   llvm::StructType *sTy) {
  // fill in any gaps, which are the explicit padding that swiftc inserts.
  for (unsigned i = 0, e = elements.size(); i != e; ++i) {
    if (elements[i].empty()) {
      auto *eltTy = sTy->getElementType(i);
      assert(eltTy->isArrayTy() &&
             eltTy->getArrayElementType()->isIntegerTy(8) &&
             "Unexpected non-byte-array type for constant struct padding");
      elements[i].add(llvm::UndefValue::get(eltTy));
    }
  }
}

/// Creates a struct which contains all values of `explosions`.
///
/// If all explosions have a single element and those elements match the
/// elements of `structTy`, it uses this type as result type.
/// Otherwise, it creates an anonymous struct. This can be the case for enums.
llvm::Constant *createStructFromExplosion(SmallVectorImpl<Explosion> &explosions,
                                          llvm::StructType *structTy) {
  assert(explosions.size() == structTy->getNumElements());
  bool canUseStructType = true;
  llvm::SmallVector<llvm::Constant *, 32> values;
  unsigned idx = 0;
  for (auto &elmt : explosions) {
    if (elmt.size() != 1)
      canUseStructType = false;
    for (llvm::Value *v : elmt.claimAll()) {
      if (v->getType() != structTy->getElementType(idx))
        canUseStructType = false;
      values.push_back(cast<llvm::Constant>(v));
    }
    idx++;
  }
  if (canUseStructType) {
    return llvm::ConstantStruct::get(structTy, values);
  } else {
    return llvm::ConstantStruct::getAnon(values, /*Packed=*/ true);
  }
}

void initWithEmptyExplosions(SmallVectorImpl<Explosion> &explosions,
                             unsigned count) {
  for (unsigned i = 0; i < count; i++) {
    explosions.push_back(Explosion());
  }
}

template <typename InstTy, typename NextIndexFunc>
Explosion emitConstantStructOrTuple(IRGenModule &IGM, InstTy inst,
                                    NextIndexFunc nextIndex, bool flatten) {
  auto type = inst->getType();
  auto *sTy = cast<llvm::StructType>(IGM.getTypeInfo(type).getStorageType());

  SmallVector<Explosion, 32> elements;
  initWithEmptyExplosions(elements, sTy->getNumElements());

  for (unsigned i = 0, e = inst->getElements().size(); i != e; ++i) {
    auto operand = inst->getOperand(i);
    llvm::Optional<unsigned> index = nextIndex(IGM, type, i);
    if (index.has_value()) {
      unsigned idx = index.value();
      assert(elements[idx].empty() &&
             "Unexpected constant struct field overlap");
      elements[idx] = emitConstantValue(IGM, operand, flatten);
    }
  }
  if (flatten) {
    Explosion out;
    for (auto &elmt : elements) {
      out.add(elmt.claimAll());
    }
    return out;
  }
  insertPadding(elements, sTy);
  return createStructFromExplosion(elements, sTy);
}
} // end anonymous namespace

/// Returns the usub_with_overflow builtin if \p TE extracts the result of
/// such a subtraction, which is required to have an integer_literal as right
/// operand.
static BuiltinInst *getOffsetSubtract(const TupleExtractInst *TE, SILModule &M) {
  // Match the pattern:
  // tuple_extract(usub_with_overflow(x, integer_literal, integer_literal 0), 0)

  if (TE->getFieldIndex() != 0)
    return nullptr;

  auto *BI = dyn_cast<BuiltinInst>(TE->getOperand());
  if (!BI)
    return nullptr;
  if (M.getBuiltinInfo(BI->getName()).ID != BuiltinValueKind::USubOver)
    return nullptr;

  if (!isa<IntegerLiteralInst>(BI->getArguments()[1]))
    return nullptr;

  auto *overflowFlag = dyn_cast<IntegerLiteralInst>(BI->getArguments()[2]);
  if (!overflowFlag || !overflowFlag->getValue().isZero())
    return nullptr;

  return BI;
}

static bool isPowerOfTwo(unsigned x) {
  return (x & -x) == x;
}

/// Replace i24, i40, i48 and i56 constants in `e` with the corresponding byte values.
/// Such unaligned integer constants are not correctly layed out in the data section.
static Explosion replaceUnalignedIntegerValues(IRGenModule &IGM, Explosion e) {
  Explosion out;
  while (!e.empty()) {
    llvm::Value *v = e.claimNext();

    if (auto *constInt = dyn_cast<llvm::ConstantInt>(v)) {
      unsigned size = constInt->getBitWidth();
      if (size % 8 == 0 && !isPowerOfTwo(size)) {
        BitPatternReader reader(constInt->getValue(), IGM.Triple.isLittleEndian());
        while (size > 0) {
          APInt byte = reader.read(8);
          out.add(llvm::ConstantInt::get(IGM.getLLVMContext(), byte));
          size -= 8;
        }
        continue;
      }
    }
    out.add(v);
  }
  return out;
}

Explosion irgen::emitConstantValue(IRGenModule &IGM, SILValue operand,
                                   bool flatten) {
  if (auto *SI = dyn_cast<StructInst>(operand)) {
    // The only way to get a struct's stored properties (which we need to map to
    // their physical/LLVM index) is to iterate over the properties
    // progressively. Fortunately the iteration order matches the order of
    // operands in a StructInst.
    auto StoredProperties = SI->getStructDecl()->getStoredProperties();
    auto Iter = StoredProperties.begin();

    return emitConstantStructOrTuple(
        IGM, SI, [&Iter](IRGenModule &IGM, SILType Type, unsigned _i) mutable {
          (void)_i;
          auto *FD = *Iter++;
          return irgen::getPhysicalStructFieldIndex(IGM, Type, FD);
        }, flatten);
  } else if (auto *TI = dyn_cast<TupleInst>(operand)) {
    return emitConstantStructOrTuple(IGM, TI,
                                     irgen::getPhysicalTupleElementStructIndex,
                                     flatten);
  } else if (auto *ei = dyn_cast<EnumInst>(operand)) {
    Explosion data;
    if (ei->hasOperand()) {
      data = emitConstantValue(IGM, ei->getOperand(), /*flatten=*/ true);
    }
    // Use `emitValueInjection` to create the enum constant.
    // Usually this method creates code in the current function. But if all
    // arguments to the enum are constant, the builder never has to emit an
    // instruction. Instead it can constant fold everything and just returns
    // the final constant.
    Explosion out;
    IRBuilder builder(IGM.getLLVMContext(), false);
    getEnumImplStrategy(IGM, ei->getType())
      .emitValueInjection(IGM, builder, ei->getElement(), data, out);
    return replaceUnalignedIntegerValues(IGM, std::move(out));
  } else if (auto *ILI = dyn_cast<IntegerLiteralInst>(operand)) {
    return emitConstantInt(IGM, ILI);
  } else if (auto *FLI = dyn_cast<FloatLiteralInst>(operand)) {
    return emitConstantFP(IGM, FLI);
  } else if (auto *SLI = dyn_cast<StringLiteralInst>(operand)) {
    return emitAddrOfConstantString(IGM, SLI);
  } else if (auto *BI = dyn_cast<BuiltinInst>(operand)) {
    auto args = BI->getArguments();
    switch (IGM.getSILModule().getBuiltinInfo(BI->getName()).ID) {
      case BuiltinValueKind::ZeroInitializer:
        return emitConstantZero(IGM, BI);
      case BuiltinValueKind::PtrToInt: {
        auto *ptr = emitConstantValue(IGM, args[0]).claimNextConstant();
        return llvm::ConstantExpr::getPtrToInt(ptr, IGM.IntPtrTy);
      }
      case BuiltinValueKind::IntToPtr: {
        auto *num = emitConstantValue(IGM, args[0]).claimNextConstant();
        return llvm::ConstantExpr::getIntToPtr(num, IGM.Int8PtrTy);
      }
      case BuiltinValueKind::ZExtOrBitCast: {
        auto *val = emitConstantValue(IGM, args[0]).claimNextConstant();
        return llvm::ConstantExpr::getZExtOrBitCast(val,
                                                    IGM.getStorageType(BI->getType()));
      }
      case BuiltinValueKind::StringObjectOr: {
        // It is a requirement that the or'd bits in the left argument are
        // initialized with 0. Therefore the or-operation is equivalent to an
        // addition. We need an addition to generate a valid relocation.
        auto *rhs = emitConstantValue(IGM, args[1]).claimNextConstant();
        if (auto *TE = dyn_cast<TupleExtractInst>(args[0])) {
          // Handle StringObjectOr(tuple_extract(usub_with_overflow(x, offset)), bits)
          // This pattern appears in UTF8 String literal construction.
          // Generate the equivalent: add(x, sub(bits - offset)
          BuiltinInst *SubtrBI = getOffsetSubtract(TE, IGM.getSILModule());
          assert(SubtrBI && "unsupported argument of StringObjectOr");
          auto subArgs = SubtrBI->getArguments();
          auto *ptr = emitConstantValue(IGM, subArgs[0]).claimNextConstant();
          auto *offset = emitConstantValue(IGM, subArgs[1]).claimNextConstant();
          auto *totalOffset = llvm::ConstantExpr::getSub(rhs, offset);
          return llvm::ConstantExpr::getAdd(ptr, totalOffset);
        }
        auto *lhs = emitConstantValue(IGM, args[0]).claimNextConstant();
        return llvm::ConstantExpr::getAdd(lhs, rhs);
      }
      default:
        llvm_unreachable("unsupported builtin for constant expression");
    }
  } else if (auto *VTBI = dyn_cast<ValueToBridgeObjectInst>(operand)) {
    auto *val = emitConstantValue(IGM, VTBI->getOperand()).claimNextConstant();
    auto *sTy = IGM.getTypeInfo(VTBI->getType()).getStorageType();
    return llvm::ConstantExpr::getIntToPtr(val, sTy);

  } else if (auto *CFI = dyn_cast<ConvertFunctionInst>(operand)) {
    return emitConstantValue(IGM, CFI->getOperand());

  } else if (auto *T2TFI = dyn_cast<ThinToThickFunctionInst>(operand)) {
    SILType type = operand->getType();
    auto *sTy = cast<llvm::StructType>(IGM.getTypeInfo(type).getStorageType());

    auto *function = llvm::ConstantExpr::getBitCast(
        emitConstantValue(IGM, T2TFI->getCallee()).claimNextConstant(),
        sTy->getTypeAtIndex((unsigned)0));

    auto *context = llvm::ConstantExpr::getBitCast(
        llvm::ConstantPointerNull::get(IGM.OpaquePtrTy),
        sTy->getTypeAtIndex((unsigned)1));

    if (flatten) {
      Explosion out;
      out.add({function, context});
      return out;
    }
    return llvm::ConstantStruct::get(sTy, {function, context});

  } else if (auto *FRI = dyn_cast<FunctionRefInst>(operand)) {
    SILFunction *fn = FRI->getReferencedFunction();

    llvm::Constant *fnPtr = IGM.getAddrOfSILFunction(fn, NotForDefinition);
    assert(!fn->isAsync() && "TODO: support async functions");

    CanSILFunctionType fnType = FRI->getType().getAs<SILFunctionType>();
    auto authInfo = PointerAuthInfo::forFunctionPointer(IGM, fnType);
    if (authInfo.isSigned()) {
      auto constantDiscriminator =
          cast<llvm::Constant>(authInfo.getDiscriminator());
      assert(!constantDiscriminator->getType()->isPointerTy());
      fnPtr = IGM.getConstantSignedPointer(fnPtr, authInfo.getKey(), nullptr,
        constantDiscriminator);
    }
    llvm::Type *ty = IGM.getTypeInfo(FRI->getType()).getStorageType();
    fnPtr = llvm::ConstantExpr::getBitCast(fnPtr, ty);
    return fnPtr;
  } else {
    llvm_unreachable("Unsupported SILInstruction in static initializer!");
  }
}

llvm::Constant *irgen::emitConstantObject(IRGenModule &IGM, ObjectInst *OI,
                                         StructLayout *ClassLayout) {
  auto *sTy = cast<llvm::StructType>(ClassLayout->getType());

  SmallVector<Explosion, 32> elements;
  initWithEmptyExplosions(elements, sTy->getNumElements());

  unsigned NumElems = OI->getAllElements().size();
  assert(NumElems == ClassLayout->getElements().size());

  // Construct the object init value including tail allocated elements.
  for (unsigned i = 0; i != NumElems; ++i) {
    SILValue Val = OI->getAllElements()[i];
    const ElementLayout &EL = ClassLayout->getElements()[i];
    if (!EL.isEmpty()) {
      unsigned idx = EL.getStructIndex();
      assert(idx != 0 && "the first element is the object header");
      assert(elements[idx].empty() &&
             "Unexpected constant struct field overlap");
      elements[idx] = emitConstantValue(IGM, Val);
    }
  }
  // Construct the object header.
  llvm::StructType *ObjectHeaderTy = cast<llvm::StructType>(sTy->getElementType(0));

  if (IGM.canMakeStaticObjectsReadOnly()) {
    if (!IGM.swiftImmortalRefCount) {
      auto *var = new llvm::GlobalVariable(IGM.Module, IGM.Int8Ty,
                                        /*constant*/ true, llvm::GlobalValue::ExternalLinkage,
                                        /*initializer*/ nullptr, "_swiftImmortalRefCount");
      IGM.swiftImmortalRefCount = var;
    }
    if (!IGM.swiftStaticArrayMetadata) {

      // Static arrays can only contain trivial elements. Therefore we can reuse
      // the metadata of the empty array buffer. The important thing is that its
      // deinit is a no-op and does not actually destroy any elements.
      auto *var = new llvm::GlobalVariable(IGM.Module, IGM.TypeMetadataStructTy,
                                        /*constant*/ true, llvm::GlobalValue::ExternalLinkage,
                                        /*initializer*/ nullptr, "$ss19__EmptyArrayStorageCN");
      IGM.swiftStaticArrayMetadata = var;
    }
    elements[0].add(llvm::ConstantStruct::get(ObjectHeaderTy, {
      IGM.swiftStaticArrayMetadata,
      llvm::ConstantExpr::getPtrToInt(IGM.swiftImmortalRefCount, IGM.IntPtrTy)}));
  } else {
    elements[0].add(llvm::Constant::getNullValue(ObjectHeaderTy));
  }
  insertPadding(elements, sTy);
  return createStructFromExplosion(elements, sTy);
}

void ConstantAggregateBuilderBase::addUniqueHash(StringRef data) {
  llvm::BLAKE3 hasher;
  hasher.update(data);
  auto rawHash = hasher.final();
  auto truncHash = llvm::makeArrayRef(rawHash).slice(0, NumBytes_UniqueHash);
  add(llvm::ConstantDataArray::get(IGM().getLLVMContext(), truncHash));
}
