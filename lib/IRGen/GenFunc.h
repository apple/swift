//===--- GenFunc.h - Swift IR generation for functions ----------*- C++ -*-===//
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
//  This file provides the private interface to the function and
//  function-type emission code.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_GENFUNC_H
#define SWIFT_IRGEN_GENFUNC_H

#include "swift/AST/Types.h"

namespace llvm {
  class Function;
  class Value;
}

namespace swift {
namespace irgen {
  class Address;
  class Explosion;
  class ForeignFunctionInfo;
  class IRGenFunction;

  /// Project the capture address from on-stack block storage.
  Address projectBlockStorageCapture(IRGenFunction &IGF,
                                     Address storageAddr,
                                     CanSILBlockStorageType storageTy);
  
  /// Emit the block header into a block storage slot.
  void emitBlockHeader(IRGenFunction &IGF,
                       Address storage,
                       CanSILBlockStorageType blockTy,
                       llvm::Constant *invokeFunction,
                       CanSILFunctionType invokeTy,
                       ForeignFunctionInfo foreignInfo);

  /// Emit a partial application thunk for a function pointer applied to a
  /// partial set of argument values.
  llvm::Optional<StackAddress> emitFunctionPartialApplication(
      IRGenFunction &IGF, SILFunction &SILFn, const FunctionPointer &fnPtr,
      llvm::Value *fnContext, Explosion &args,
      ArrayRef<SILParameterInfo> argTypes, SubstitutionMap subs,
      CanSILFunctionType origType, CanSILFunctionType substType,
      CanSILFunctionType outType, Explosion &out, bool isOutlined);
  CanType getArgumentLoweringType(CanType type, SILParameterInfo paramInfo,
                                  bool isNoEscape);
} // end namespace irgen
} // end namespace swift

#endif
