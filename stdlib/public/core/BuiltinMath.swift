//===--- BuiltinMath.swift --------------------------------*- swift -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

// Each of the following functions is ordered to match math.h

// These functions have a corresponding LLVM intrinsic.
// Note, keep this up to date with Darwin/tgmath.swift.gyb

// Order of intrinsics: cos, sin, exp, exp2, log, log10, log2, nearbyint, rint

// Float Intrinsics (32 bits)

@_transparent
public func _cos(_ x: Float) -> Float {
  return Float(Builtin.int_cos_FPIEEE32(x._value))
}

@_transparent
public func _sin(_ x: Float) -> Float {
  return Float(Builtin.int_sin_FPIEEE32(x._value))
}

@_transparent
public func _exp(_ x: Float) -> Float {
  return Float(Builtin.int_exp_FPIEEE32(x._value))
}

@_transparent
public func _exp2(_ x: Float) -> Float {
  return Float(Builtin.int_exp2_FPIEEE32(x._value))
}

@_transparent
public func _log(_ x: Float) -> Float {
  return Float(Builtin.int_log_FPIEEE32(x._value))
}

@_transparent
public func _log10(_ x: Float) -> Float {
  return Float(Builtin.int_log10_FPIEEE32(x._value))
}

@_transparent
public func _log2(_ x: Float) -> Float {
  return Float(Builtin.int_log2_FPIEEE32(x._value))
}

@_transparent
public func _nearbyint(_ x: Float) -> Float {
  return Float(Builtin.int_nearbyint_FPIEEE32(x._value))
}

@_transparent
public func _rint(_ x: Float) -> Float {
  return Float(Builtin.int_rint_FPIEEE32(x._value))
}

// Double Intrinsics (64 bits)

@_transparent
public func _cos(_ x: Double) -> Double {
  return Double(Builtin.int_cos_FPIEEE64(x._value))
}

@_transparent
public func _sin(_ x: Double) -> Double {
  return Double(Builtin.int_sin_FPIEEE64(x._value))
}

@_transparent
public func _exp(_ x: Double) -> Double {
  return Double(Builtin.int_exp_FPIEEE64(x._value))
}

@_transparent
public func _exp2(_ x: Double) -> Double {
  return Double(Builtin.int_exp2_FPIEEE64(x._value))
}

@_transparent
public func _log(_ x: Double) -> Double {
  return Double(Builtin.int_log_FPIEEE64(x._value))
}

@_transparent
public func _log10(_ x: Double) -> Double {
  return Double(Builtin.int_log10_FPIEEE64(x._value))
}

@_transparent
public func _log2(_ x: Double) -> Double {
  return Double(Builtin.int_log2_FPIEEE64(x._value))
}

@_transparent
public func _nearbyint(_ x: Double) -> Double {
  return Double(Builtin.int_nearbyint_FPIEEE64(x._value))
}

@_transparent
public func _rint(_ x: Double) -> Double {
  return Double(Builtin.int_rint_FPIEEE64(x._value))
}

// Float80 Intrinsics (80 bits)

#if !(os(Windows) || os(Android) || os(FreeBSD)) && (arch(i386) || arch(x86_64))
@_transparent
public func _cos(_ x: Float80) -> Float80 {
  return Float80(Builtin.int_cos_FPIEEE80(x._value))
}

@_transparent
public func _sin(_ x: Float80) -> Float80 {
  return Float80(Builtin.int_sin_FPIEEE80(x._value))
}

@_transparent
public func _exp(_ x: Float80) -> Float80 {
  return Float80(Builtin.int_exp_FPIEEE80(x._value))
}

@_transparent
public func _exp2(_ x: Float80) -> Float80 {
  return Float80(Builtin.int_exp2_FPIEEE80(x._value))
}

@_transparent
public func _log(_ x: Float80) -> Float80 {
  return Float80(Builtin.int_log_FPIEEE80(x._value))
}

@_transparent
public func _log10(_ x: Float80) -> Float80 {
  return Float80(Builtin.int_log10_FPIEEE80(x._value))
}

@_transparent
public func _log2(_ x: Float80) -> Float80 {
  return Float80(Builtin.int_log2_FPIEEE80(x._value))
}

@_transparent
public func _nearbyint(_ x: Float80) -> Float80 {
  return Float80(Builtin.int_nearbyint_FPIEEE80(x._value))
}

@_transparent
public func _rint(_ x: Float80) -> Float80 {
  return Float80(Builtin.int_rint_FPIEEE80(x._value))
}
#endif
