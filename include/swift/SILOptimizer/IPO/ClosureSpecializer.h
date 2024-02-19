//===-------------------------- ClosureSpecializer.h ------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===-----------------------------------------------------------------------------===//
#ifndef SWIFT_SILOPTIMIZER_CLOSURESPECIALIZER_H
#define SWIFT_SILOPTIMIZER_CLOSURESPECIALIZER_H

#include "swift/SIL/SILFunction.h"

namespace swift {

/// If \p function is a function-signature specialization for a constant-
/// propagated function argument, returns 1.
/// If \p function is a specialization of such a specialization, returns 2.
/// And so on.
int getSpecializationLevel(SILFunction *f);

enum class AutoDiffFunctionComponent : char { JVP = 'f', VJP = 'r' };

/// Returns true if the function is the JVP or the VJP corresponding to
/// a differentiable function.
bool isDifferentiableFuncComponent(
    SILFunction *f,
    AutoDiffFunctionComponent component = AutoDiffFunctionComponent::VJP);

} // namespace swift
#endif