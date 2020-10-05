//===--- Transforms.h -----------------------------------------------------===//
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
///
/// \file
///
/// Top level transforms for SemanticARCOpts
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILOPTIMIZER_SEMANTICARCOPT_TRANSFORMS_H
#define SWIFT_SILOPTIMIZER_SEMANTICARCOPT_TRANSFORMS_H

#include "llvm/Support/Compiler.h"

namespace swift {
namespace semanticarc {

struct Context;

/// Given the current map of owned phi arguments to consumed incoming values in
/// ctx, attempt to convert these owned phi arguments to guaranteed phi
/// arguments if the phi arguments are the only thing that kept us from
/// converting these incoming values to be guaranteed.
///
/// \returns true if we converted atleast one phi from owned -> guaranteed and
/// eliminated ARC traffic as a result.
bool tryConvertOwnedPhisToGuaranteedPhis(Context &ctx) LLVM_LIBRARY_VISIBILITY;

/// In certain cases, we have tightly scoped trivial (1) coroutines that yield
/// guaranteed values that are immediately copied before the coroutine's
/// lifetime ends. In such a case, if the copy is never consumed, lifetime
/// extend the coroutine over the uses of the copy and eliminate the copy.
///
/// (1) A "trivial" coroutine is a coroutine that only yields once and has
/// resume/abort blocks without side-effects. This allows us to extend the
/// lifetime of the coroutine without needing to worry about the coroutine's
/// resume/abort blocks causing side-effects.
///
/// NOTE: This is intended to be run after canonicalizing the IR by eliminating
/// redundant copies/@owned phis.
///
/// FIXME: Teach this how to lifetime extend over phi webs.
bool tryEliminatingCopiesByLifetimeExtendingTrivialCoroutines(Context &ctx)
    LLVM_LIBRARY_VISIBILITY;

} // namespace semanticarc
} // namespace swift

#endif
