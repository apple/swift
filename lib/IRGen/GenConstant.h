//===--- GenConstant.h - Swift IR Generation For Constants ------*- C++ -*-===//
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

#ifndef SWIFT_IRGEN_GENCONSTANT_H
#define SWIFT_IRGEN_GENCONSTANT_H

#include "llvm/IR/Constant.h"

#include "IRGenModule.h"

namespace swift {
namespace irgen {

/// Construct a ConstantInt from an IntegerLiteralInst.
llvm::Constant *emitConstantInt(IRGenModule &IGM, IntegerLiteralInst *ILI);

/// Construct a zero from a zero initializer BuiltinInst.
llvm::Constant *emitConstantZero(IRGenModule &IGM, BuiltinInst *Bi);

/// Construct a ConstantFP from a FloatLiteralInst.
llvm::Constant *emitConstantFP(IRGenModule &IGM, FloatLiteralInst *FLI);

/// Construct a pointer to a string from a StringLiteralInst.
llvm::Constant *emitAddrOfConstantString(IRGenModule &IGM,
                                         StringLiteralInst *SLI);

/// Construct a constant from a SILValue containing constant values.
Explosion emitConstantValue(IRGenModule &IGM, SILValue value,
                            bool flatten = false);

/// Construct an object (with a HeapObject header) from an ObjectInst
/// containing constant values.
llvm::Constant *emitConstantObject(IRGenModule &IGM, ObjectInst *OI,
                                   StructLayout *ClassLayout);
}
}

#endif
