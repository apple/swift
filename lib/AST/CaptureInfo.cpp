//===--- CaptureInfo.cpp - Data Structure for Capture Lists ---------------===//
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

#include "swift/AST/CaptureInfo.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/Basic/Assertions.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;

CapturedValue::CapturedValue(Expr *Val, unsigned Flags)
    : Value(Val, Flags), Loc(SourceLoc()) {
  assert(isa<OpaqueValueExpr>(Val) || isa<PackElementExpr>(Val));
}

bool CapturedValue::isPackElement() const {
  return isExpr() && isa<PackElementExpr>(getExpr());
}
bool CapturedValue::isOpaqueValue() const {
  return isExpr() && isa<OpaqueValueExpr>(getExpr());
}

OpaqueValueExpr *CapturedValue::getOpaqueValue() const {
  return dyn_cast_or_null<OpaqueValueExpr>(getExpr());
}

PackElementExpr *CapturedValue::getPackElement() const {
  return dyn_cast_or_null<PackElementExpr>(getExpr());
}

Type CapturedValue::getPackElementType() const {
  return getPackElement()->getType();
}

ArrayRef<CapturedValue>
CaptureInfo::CaptureInfoStorage::getCaptures() const {
  return llvm::ArrayRef(this->getTrailingObjects<CapturedValue>(), NumCapturedValues);
}

ArrayRef<GenericEnvironment *>
CaptureInfo::CaptureInfoStorage::getGenericEnvironments() const {
  return llvm::ArrayRef(this->getTrailingObjects<GenericEnvironment *>(), NumGenericEnvironments);
}

//===----------------------------------------------------------------------===//
//                             MARK: CaptureInfo
//===----------------------------------------------------------------------===//

CaptureInfo::CaptureInfo(ASTContext &ctx, ArrayRef<CapturedValue> captures,
                         DynamicSelfType *dynamicSelf,
                         OpaqueValueExpr *opaqueValue,
                         bool genericParamCaptures,
                         ArrayRef<GenericEnvironment *> genericEnv) {
  static_assert(IsTriviallyDestructible<CapturedValue>::value,
                "Capture info is alloc'd on the ASTContext and not destroyed");
  static_assert(IsTriviallyDestructible<CaptureInfo::CaptureInfoStorage>::value,
                "Capture info is alloc'd on the ASTContext and not destroyed");

  // This is the only kind of local generic environment we can capture right now.
#ifndef NDEBUG
  for (auto *env : genericEnv) {
    assert(env->getKind() == GenericEnvironment::Kind::OpenedElement);
  }
#endif

  OptionSet<Flags> flags;
  if (genericParamCaptures)
    flags |= Flags::HasGenericParamCaptures;

  if (captures.empty() && genericEnv.empty() && !dynamicSelf && !opaqueValue) {
    *this = CaptureInfo::empty();
    StorageAndFlags.setInt(flags);
    return;
  }

  size_t storageToAlloc =
      CaptureInfoStorage::totalSizeToAlloc<CapturedValue,
                                           GenericEnvironment *>(captures.size(),
                                                                 genericEnv.size());
  void *storageBuf = ctx.Allocate(storageToAlloc, alignof(CaptureInfoStorage));
  auto *storage = new (storageBuf) CaptureInfoStorage(dynamicSelf,
                                                      opaqueValue,
                                                      captures.size(),
                                                      genericEnv.size());
  StorageAndFlags.setPointerAndInt(storage, flags);
  std::uninitialized_copy(captures.begin(), captures.end(),
                          storage->getTrailingObjects<CapturedValue>());
  std::uninitialized_copy(genericEnv.begin(), genericEnv.end(),
                          storage->getTrailingObjects<GenericEnvironment *>());
}

CaptureInfo CaptureInfo::empty() {
  static const CaptureInfoStorage empty{/*dynamicSelf*/nullptr,
                                        /*opaqueValue*/nullptr,
                                        0, 0};
  CaptureInfo result;
  result.StorageAndFlags.setPointer(&empty);
  return result;
}

VarDecl *CaptureInfo::getIsolatedParamCapture() const {
  for (const auto &capture : getCaptures()) {
    // NOTE: isLocalCapture() returns false if we have dynamic self metadata
    // since dynamic self metadata is never an isolated capture. So we can just
    // call isLocalCapture without checking for dynamic self metadata.
    if (!capture.isLocalCapture())
      continue;

    // If we captured an isolated parameter, return it.
    if (auto param = dyn_cast_or_null<ParamDecl>(capture.getDecl())) {
      // If we have captured an isolated parameter, return it.
      if (param->isIsolated())
        return param;
    }

    // If we captured 'self', check whether it is (still) isolated.
    if (auto var = dyn_cast_or_null<VarDecl>(capture.getDecl())) {
      if (var->isSelfParamCapture() && var->isSelfParamCaptureIsolated())
        return var;
    }
  }

  return nullptr;
}

LLVM_ATTRIBUTE_USED void CaptureInfo::dump() const {
  print(llvm::errs());
  llvm::errs() << '\n';
}

void CaptureInfo::print(raw_ostream &OS) const {
  OS << "captures=(";

  if (hasGenericParamCaptures())
    OS << "<generic> ";
  if (hasDynamicSelfCapture())
    OS << "<dynamic_self> ";
  if (hasOpaqueValueCapture())
    OS << "<opaque_value> ";

  interleave(getCaptures(),
             [&](const CapturedValue &capture) {
               if (capture.getDecl())
                 OS << capture.getDecl()->getBaseName();
               else if (capture.isPackElement()) {
                 OS << "[pack element] ";
                 capture.getPackElement()->dump(OS);
               } else if (capture.isOpaqueValue()) {
                 OS << "[opaque] ";
                 capture.getOpaqueValue()->dump(OS);
               } else {
                 OS << "[unknown] ";
                 assert(false);
               }

               if (capture.isDirect())
                 OS << "<direct>";
               if (capture.isNoEscape())
                 OS << "<noescape>";
             },
             [&] { OS << ", "; });

  interleave(getGenericEnvironments(),
             [&](GenericEnvironment *genericEnv) {
               OS << " shape_class=";
               OS << genericEnv->getOpenedElementShapeClass();
             },
             [&] { OS << ","; });
  OS << ')';
}

//===----------------------------------------------------------------------===//
//                            MARK: CapturedValue
//===----------------------------------------------------------------------===//

bool CapturedValue::isLocalCapture() const {
  auto *decl = Value.getPointer().dyn_cast<ValueDecl *>();
  return decl && decl->isLocalCapture();
}
