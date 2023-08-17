//===--- AssumeSingleThreaded.swift - Assume single-threaded execution ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Assume that user code is single-threaded.
//
// Convert all reference counting operations into non-atomic ones.
//
// To get rid of most atomic reference counting operations, the standard
// library should be compiled in this mode as well .
//
// This pass affects only reference counting operations resulting from SIL
// instructions. It wouldn't affect places in the runtime C++ code which
// hard-code calls to retain/release. We could take advantage of the Instruments
// instrumentation stubs to redirect calls from the runtime if it was
// significant, or else just build a single-threaded variant of the runtime.
//
//===----------------------------------------------------------------------===//

import SIL

let assumeSingleThreadedPass = FunctionPass(name: "sil-assume-single-threaded") {
  (function: Function, context: FunctionPassContext) in

  for inst in function.instructions {
    guard let rcInst = inst as? RefCountingInst else { continue }

    rcInst.setAtomicity(isAtomic: false, context)
  }
}
