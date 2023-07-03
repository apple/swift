//===--- Callee.h -----------------------------------------------*- C++ -*-===//
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

#ifndef SWIFT_SILGEN_CALLEE_H
#define SWIFT_SILGEN_CALLEE_H

#include "swift/AST/ForeignAsyncConvention.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/ForeignInfo.h"
#include "swift/AST/Types.h"
#include "swift/SIL/AbstractionPattern.h"

namespace swift {
namespace Lowering {

class CalleeTypeInfo {
public:
  llvm::Optional<AbstractionPattern> origFormalType;
  CanSILFunctionType substFnType;
  llvm::Optional<AbstractionPattern> origResultType;
  CanType substResultType;
  ForeignInfo foreign;

private:
  llvm::Optional<SILFunctionTypeRepresentation> overrideRep;

public:
  CalleeTypeInfo() = default;

  CalleeTypeInfo(
      CanSILFunctionType substFnType, AbstractionPattern origResultType,
      CanType substResultType,
      const llvm::Optional<ForeignErrorConvention> &foreignError,
      const llvm::Optional<ForeignAsyncConvention> &foreignAsync,
      ImportAsMemberStatus foreignSelf,
      llvm::Optional<SILFunctionTypeRepresentation> overrideRep = llvm::None)
      : origFormalType(llvm::None), substFnType(substFnType),
        origResultType(origResultType),
        substResultType(substResultType), foreign{foreignSelf, foreignError,
                                                  foreignAsync},
        overrideRep(overrideRep) {}

  CalleeTypeInfo(
      CanSILFunctionType substFnType, AbstractionPattern origResultType,
      CanType substResultType,
      llvm::Optional<SILFunctionTypeRepresentation> overrideRep = llvm::None)
      : origFormalType(llvm::None), substFnType(substFnType),
        origResultType(origResultType), substResultType(substResultType),
        foreign(), overrideRep(overrideRep) {}

  SILFunctionTypeRepresentation getOverrideRep() const {
    return overrideRep.value_or(substFnType->getRepresentation());
  }
};

} // namespace Lowering
} // namespace swift

#endif
