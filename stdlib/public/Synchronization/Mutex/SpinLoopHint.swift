//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift Atomics open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#if arch(arm) || arch(arm64) || arch(arm64_32)

@inline(__always)
var _tries: Int {
  100
}

#if arch(arm)

// The following are acceptable operands to the aarch64 hint intrinsic from
// 'llvm-project/llvm/lib/Target/ARM/ARMInstrInfo.td':
//
// `nop`   = 0
// `yield` = 1
// `wfe`   = 2
// `wfi`   = 3
// `sev`   = 4
// `sevl`  = 5
//
// There are others, but for the sake of spin loops, we only care about 'wfe'.
@_extern(c, "llvm.arm.hint")
func _hint(_: UInt32)

#else

// The following are acceptable operands to the aarch64 hint intrinsic from
// 'llvm-project/llvm/lib/Target/AArch64/AArch64InstrInfo.td':
//
// `nop`   = 0
// `yield` = 1
// `wfe`   = 2
// `wfi`   = 3
// `sev`   = 4
// `sevl`  = 5
//
// There are others, but for the sake of spin loops, we only care about 'wfe'.
@_extern(c, "llvm.aarch64.hint")
func _hint(_: UInt32)

#endif

@inline(__always)
func _wfe() {
  _hint(2)
}

#elseif arch(i386) || arch(x86_64)

@inline(__always)
var _tries: Int {
  1000
}

@_extern(c, "llvm.x86.sse2.pause")
func _pause()

#else

@inline(__always)
var _tries: Int {
  100
}

#endif

@inline(__always)
func _spinLoopHint() {
#if arch(arm) || arch(arm64) || arch(arm64_32)
  _wfe()
#elseif arch(i386) || arch(x86_64)
  _pause()
#else
  // Just do a nop on architectures we don't know about.
#endif
}
