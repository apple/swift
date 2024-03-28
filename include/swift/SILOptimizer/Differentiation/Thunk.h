//===--- Thunk.h - Automatic differentiation thunks -----------*- C++ -*---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Automatic differentiation thunk generation utilities.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_THUNK_H
#define SWIFT_SILOPTIMIZER_UTILS_DIFFERENTIATION_THUNK_H

#include "swift/AST/AutoDiff.h"
#include "swift/Basic/LLVM.h"
#include "swift/SIL/SILBuilder.h"

namespace swift {

class SILOptFunctionBuilder;
class SILModule;
class SILLocation;
class SILValue;
class OpenedArchetypeType;
class GenericEnvironment;
class SubstitutionMap;
class ArchetypeType;

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

namespace autodiff {

class ADContext;

//===----------------------------------------------------------------------===//
// Thunk helpers
//===----------------------------------------------------------------------===//
// These helpers are copied/adapted from SILGen. They should be refactored and
// moved to a shared location.
//===----------------------------------------------------------------------===//


/// Get or create a reabstraction thunk from `fromType` to `toType`, to be
/// called in `caller`.
SILFunction *getOrCreateReabstractionThunk(SILOptFunctionBuilder &fb,
                                           SILModule &module, SILLocation loc,
                                           SILFunction *caller,
                                           CanSILFunctionType fromType,
                                           CanSILFunctionType toType);

SILValue reabstractCoroutine(
    SILBuilder &builder, SILOptFunctionBuilder &fb, SILLocation loc,
    SILValue fn, CanSILFunctionType toType,
    std::function<SubstitutionMap(SubstitutionMap)> remapSubstitutions);

/// Reabstracts the given function-typed value `fn` to the target type `toType`.
/// Remaps substitutions using `remapSubstitutions`.
SILValue reabstractFunction(
    SILBuilder &builder, SILOptFunctionBuilder &fb, SILLocation loc,
    SILValue fn, CanSILFunctionType toType,
    std::function<SubstitutionMap(SubstitutionMap)> remapSubstitutions);

/// Get or create a derivative function parameter index subset thunk from
/// `actualIndices` to `desiredIndices` for the given associated function
/// value and original function operand. Returns a pair of the parameter
/// index subset thunk and its interface substitution map (used to partially
/// apply the thunk).
/// Calls `getOrCreateSubsetParametersThunkForLinearMap` to thunk the linear
/// map returned by the derivative function.
std::pair<SILFunction *, SubstitutionMap>
getOrCreateSubsetParametersThunkForDerivativeFunction(
    SILOptFunctionBuilder &fb, SILValue origFnOperand, SILValue derivativeFn,
    AutoDiffDerivativeFunctionKind kind, const AutoDiffConfig &desiredConfig,
    const AutoDiffConfig &actualConfig, ADContext &adContext);

/// Get or create a derivative function parameter index subset thunk from
/// `actualIndices` to `desiredIndices` for the given associated function
/// value and original function operand. Returns a pair of the parameter
/// index subset thunk and its interface substitution map (used to partially
/// apply the thunk).
std::pair<SILFunction *, SubstitutionMap>
getOrCreateSubsetParametersThunkForLinearMap(
    SILOptFunctionBuilder &fb, SILFunction *assocFn,
    CanSILFunctionType origFnType, CanSILFunctionType linearMapType,
    CanSILFunctionType targetType, AutoDiffDerivativeFunctionKind kind,
    const AutoDiffConfig &desiredConfig, const AutoDiffConfig &actualConfig,
    ADContext &adContext);

} // end namespace autodiff

} // end namespace swift

#endif // SWIFT_SILOPTIMIZER_MANDATORY_DIFFERENTIATION_THUNK_H
