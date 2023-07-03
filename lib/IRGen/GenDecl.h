//===--- GenDecl.h - Swift IR generation for some decl ----------*- C++ -*-===//
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
//  This file provides the private interface to some decl emission code.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_GENDECL_H
#define SWIFT_IRGEN_GENDECL_H

#include "DebugTypeInfo.h"
#include "IRGen.h"
#include "swift/Basic/OptimizationMode.h"
#include "swift/SIL/SILLocation.h"
#include "clang/AST/DeclCXX.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/Support/CommandLine.h"

namespace llvm {
  class AttributeList;
  class Function;
  class FunctionType;
  class CallBase;
}
namespace swift {
namespace irgen {
  class IRGenModule;
  class LinkEntity;
  class LinkInfo;
  class Signature;

  void updateLinkageForDefinition(IRGenModule &IGM,
                                  llvm::GlobalValue *global,
                                  const LinkEntity &entity);

  llvm::Function *createFunction(
      IRGenModule &IGM, LinkInfo &linkInfo, const Signature &signature,
      llvm::Function *insertBefore = nullptr,
      OptimizationMode FuncOptMode = OptimizationMode::NotSet,
      StackProtectorMode stackProtect = StackProtectorMode::NoStackProtector);

  llvm::GlobalVariable *
  createVariable(IRGenModule &IGM, LinkInfo &linkInfo, llvm::Type *objectType,
                 Alignment alignment, DebugTypeInfo DebugType = DebugTypeInfo(),
                 llvm::Optional<SILLocation> DebugLoc = llvm::None,
                 StringRef DebugName = StringRef());

  llvm::GlobalVariable *
  createLinkerDirectiveVariable(IRGenModule &IGM, StringRef Name);

  void disableAddressSanitizer(IRGenModule &IGM, llvm::GlobalVariable *var);

  /// If the calling convention for `ctor` doesn't match the calling convention
  /// that we assumed for it when we imported it as `initializer`, emit and
  /// return a thunk that conforms to the assumed calling convention. The thunk
  /// is marked `alwaysinline`, so it doesn't generate any runtime overhead.
  /// If the assumed calling convention was correct, just return `ctor`.
  ///
  /// See also comments in CXXMethodConventions in SIL/IR/SILFunctionType.cpp.
  llvm::Constant *
  emitCXXConstructorThunkIfNeeded(IRGenModule &IGM, Signature signature,
                                  const clang::CXXConstructorDecl *ctor,
                                  StringRef name, llvm::Constant *ctorAddress);

  llvm::CallBase *emitCXXConstructorCall(IRGenFunction &IGF,
                                         const clang::CXXConstructorDecl *ctor,
                                         llvm::FunctionType *ctorFnType,
                                         llvm::Constant *ctorAddress,
                                         llvm::ArrayRef<llvm::Value *> args);
}
}

extern llvm::cl::opt<bool> UseBasicDynamicReplacement;

#endif
