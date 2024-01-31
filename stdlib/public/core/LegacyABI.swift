//===----------------------------------------------------------------------===//
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

// This file contains non-API (or underscored) declarations that are needed to
// be kept around for ABI compatibility

extension Unicode.UTF16 {
  @available(*, unavailable, renamed: "Unicode.UTF16.isASCII")
  @inlinable
  public static func _isASCII(_ x: CodeUnit) -> Bool  {
    return Unicode.UTF16.isASCII(x)
  }
}

@available(*, unavailable, renamed: "Unicode.UTF8.isASCII")
@inlinable
internal func _isASCII(_ x: UInt8) -> Bool {
  return Unicode.UTF8.isASCII(x)
}

@available(*, unavailable, renamed: "Unicode.UTF8.isContinuation")
@inlinable
internal func _isContinuation(_ x: UInt8) -> Bool {
  return UTF8.isContinuation(x)
}

extension Substring {
@available(*, unavailable, renamed: "Substring.base")
  @inlinable
  internal var _wholeString: String { return base }
}

extension String {
  @available(*, unavailable, renamed: "String.withUTF8")
  @inlinable
  internal func _withUTF8<R>(
    _ body: (UnsafeBufferPointer<UInt8>) throws -> R
  ) rethrows -> R {
    var copy = self
    return try copy.withUTF8(body)
  }
}

extension Substring {
  @available(*, unavailable, renamed: "Substring.withUTF8")
  @inlinable
  internal func _withUTF8<R>(
    _ body: (UnsafeBufferPointer<UInt8>) throws -> R
  ) rethrows -> R {
    var copy = self
    return try copy.withUTF8(body)
  }
}

// This function is no longer used but must be kept for ABI compatibility
// because references to it may have been inlined.
@usableFromInline
internal func _branchHint(_ actual: Bool, expected: Bool) -> Bool {
  // The LLVM intrinsic underlying int_expect_Int1 now requires an immediate
  // argument for the expected value so we cannot call it here. This should
  // never be called in cases where performance matters, so just return the
  // value without any branch hint.
  return actual
}

extension String {
  @usableFromInline // Never actually used in inlinable code...
  internal func _nativeCopyUTF16CodeUnits(
    into buffer: UnsafeMutableBufferPointer<UInt16>,
    range: Range<String.Index>
  ) { fatalError() }
}

extension String.UTF16View {
  // Swift 5.x: This was accidentally shipped as inlinable, but was never used
  // from an inlinable context. The definition is kept around for technical ABI
  // compatibility (even though it shouldn't matter), but is unused.
  @inlinable @inline(__always)
  internal var _shortHeuristic: Int { return 32 }
}

// Note: These 4 functions are previous setters and modifies for
// _EachFieldOptions.classType and _EachFieldOptions.ignoreUnknown. These were
// defined as static vars which allow for shared mutable data when the intention
// was just an option for an option set which should be constant static data.

@available(SwiftStdlib 5.2, *)
@usableFromInline
@_silgen_name("$ss17_EachFieldOptionsV13ignoreUnknownABvsZ")
@_spi(Reflection)
internal func _eachFieldOptionsIgnoreUnknownSetter(_: _EachFieldOptions) {
  fatalError()
}

@available(SwiftStdlib 5.2, *)
@usableFromInline
@_silgen_name("$ss17_EachFieldOptionsV13ignoreUnknownABvMZ")
@_spi(Reflection)
internal func _eachFieldOptionsIgnoreUnknownModify(_: UnsafeRawPointer) {
  fatalError()
}

@available(SwiftStdlib 5.2, *)
@usableFromInline
@_silgen_name("$ss17_EachFieldOptionsV9classTypeABvsZ")
@_spi(Reflection)
internal func _eachFieldOptionsClassTypeSetter(_: _EachFieldOptions) {
  fatalError()
}

@available(SwiftStdlib 5.2, *)
@usableFromInline
@_silgen_name("$ss17_EachFieldOptionsV9classTypeABvMZ")
@_spi(Reflection)
internal func _eachFieldOptionsClassTypeModify(_: UnsafeRawPointer) {
  fatalError()
}
