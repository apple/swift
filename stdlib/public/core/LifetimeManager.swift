//===----------------------------------------------------------------------===//
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

/// Evaluates a closure while ensuring that the given instance is not destroyed
/// before the closure returns.
///
/// - Parameters:
///   - x: An instance to preserve until the execution of `body` is completed.
///   - body: A closure to execute that depends on the lifetime of `x` being
///     extended. If `body` has a return value, that value is also used as the
///     return value for the `withExtendedLifetime(_:_:)` method.
/// - Returns: The return value, if any, of the `body` closure parameter.
@inlinable
public func withExtendedLifetime<T, Result>(
  _ x: T, _ body: () throws -> Result
) rethrows -> Result {
  defer { _fixLifetime(x) }
  return try body()
}

@_transparent
public func withExtendedLifetime<
  T: ~Copyable & ~Escapable,
  Result: ~Copyable & ~Escapable,
  E: Error
>(
  _ x: borrowing T, _ body: () throws(E) -> Result
) throws(E) -> Result {
  defer { _fixLifetime(x) }
  return try body()
}

/// Evaluates a closure while ensuring that the given instance is not destroyed
/// before the closure returns.
///
/// - Parameters:
///   - x: An instance to preserve until the execution of `body` is completed.
///   - body: A closure to execute that depends on the lifetime of `x` being
///     extended. If `body` has a return value, that value is also used as the
///     return value for the `withExtendedLifetime(_:_:)` method.
/// - Returns: The return value, if any, of the `body` closure parameter.
@inlinable
public func withExtendedLifetime<T, Result>(
  _ x: T, _ body: (T) throws -> Result
) rethrows -> Result {
  defer { _fixLifetime(x) }
  return try body(x)
}

// Fix the lifetime of the given instruction so that the ARC optimizer does not
// shorten the lifetime of x to be before this point.
@_transparent
public func _fixLifetime<T: ~Copyable & ~Escapable>(_ x: borrowing T) {
  Builtin.fixLifetime(x)
}

/// Calls the given closure with a mutable pointer to the given argument.
///
/// The `withUnsafeMutablePointer(to:_:)` function is useful for calling
/// Objective-C APIs that take in/out parameters (and default-constructible
/// out parameters) by pointer.
///
/// The pointer argument to `body` is valid only during the execution of
/// `withUnsafeMutablePointer(to:_:)`. Do not store or return the pointer for
/// later use.
///
/// - Parameters:
///   - value: An instance to temporarily use via pointer. Note that the `inout`
///     exclusivity rules mean that, like any other `inout` argument, `value`
///     cannot be directly accessed by other code for the duration of `body`.
///     Access must only occur through the pointer argument to `body` until
///     `body` returns.
///   - body: A closure that takes a mutable pointer to `value` as its sole
///     argument. If the closure has a return value, that value is also used
///     as the return value of the `withUnsafeMutablePointer(to:_:)` function.
///     The pointer argument is valid only for the duration of the function's
///     execution.
/// - Returns: The return value, if any, of the `body` closure.
@inlinable
public func withUnsafeMutablePointer<T, Result>(
  to value: inout T,
  _ body: (UnsafeMutablePointer<T>) throws -> Result
) rethrows -> Result
{
  return try body(UnsafeMutablePointer<T>(Builtin.addressof(&value)))
}

/// Calls the given closure with a mutable pointer to the given argument.
///
/// This function is similar to `withUnsafeMutablePointer`, except that it
/// doesn't trigger stack protection for the pointer.
@_alwaysEmitIntoClient
public func _withUnprotectedUnsafeMutablePointer<T, Result>(
  to value: inout T,
  _ body: (UnsafeMutablePointer<T>) throws -> Result
) rethrows -> Result
{
#if $BuiltinUnprotectedAddressOf
  return try body(UnsafeMutablePointer<T>(Builtin.unprotectedAddressOf(&value)))
#else
  return try body(UnsafeMutablePointer<T>(Builtin.addressof(&value)))
#endif
}

/// Invokes the given closure with a pointer to the given argument.
///
/// The `withUnsafePointer(to:_:)` function is useful for calling Objective-C
/// APIs that take in parameters by const pointer.
///
/// The pointer argument to `body` is valid only during the execution of
/// `withUnsafePointer(to:_:)`. Do not store or return the pointer for later
/// use.
///
/// - Parameters:
///   - value: An instance to temporarily use via pointer.
///   - body: A closure that takes a pointer to `value` as its sole argument. If
///     the closure has a return value, that value is also used as the return
///     value of the `withUnsafePointer(to:_:)` function. The pointer argument
///     is valid only for the duration of the function's execution.
///     It is undefined behavior to try to mutate through the pointer argument
///     by converting it to `UnsafeMutablePointer` or any other mutable pointer
///     type. If you need to mutate the argument through the pointer, use
///     `withUnsafeMutablePointer(to:_:)` instead.
/// - Returns: The return value, if any, of the `body` closure.
@_alwaysEmitIntoClient
@inlinable
public func withUnsafePointer<T, E, Result>(
  to value: T,
  _ body: (UnsafePointer<T>) throws(E) -> Result
) throws(E) -> Result
{
  return try body(UnsafePointer<T>(Builtin.addressOfBorrow(value)))
}

/// ABI: Historical withUnsafePointer(to:_:) rethrows, expressed as "throws",
/// which is ABI-compatible with "rethrows".
@_silgen_name("$ss17withUnsafePointer2to_q_x_q_SPyxGKXEtKr0_lF")
@usableFromInline
func __abi_withUnsafePointer<T, Result>(
  to value: T,
  _ body: (UnsafePointer<T>) throws -> Result
) throws -> Result
{
  return try body(UnsafePointer<T>(Builtin.addressOfBorrow(value)))
}

/// Invokes the given closure with a pointer to the given argument.
///
/// The `withUnsafePointer(to:_:)` function is useful for calling Objective-C
/// APIs that take in parameters by const pointer.
///
/// The pointer argument to `body` is valid only during the execution of
/// `withUnsafePointer(to:_:)`. Do not store or return the pointer for later
/// use.
///
/// - Parameters:
///   - value: An instance to temporarily use via pointer. Note that the `inout`
///     exclusivity rules mean that, like any other `inout` argument, `value`
///     cannot be directly accessed by other code for the duration of `body`.
///     Access must only occur through the pointer argument to `body` until
///     `body` returns.
///   - body: A closure that takes a pointer to `value` as its sole argument. If
///     the closure has a return value, that value is also used as the return
///     value of the `withUnsafePointer(to:_:)` function. The pointer argument
///     is valid only for the duration of the function's execution.
///     It is undefined behavior to try to mutate through the pointer argument
///     by converting it to `UnsafeMutablePointer` or any other mutable pointer
///     type. If you need to mutate the argument through the pointer, use
///     `withUnsafeMutablePointer(to:_:)` instead.
/// - Returns: The return value, if any, of the `body` closure.
@inlinable
public func withUnsafePointer<T, Result>(
  to value: inout T,
  _ body: (UnsafePointer<T>) throws -> Result
) rethrows -> Result
{
  return try body(UnsafePointer<T>(Builtin.addressof(&value)))
}

/// Invokes the given closure with a pointer to the given argument.
///
/// This function is similar to `withUnsafePointer`, except that it
/// doesn't trigger stack protection for the pointer.
@_alwaysEmitIntoClient
public func _withUnprotectedUnsafePointer<T, Result>(
  to value: inout T,
  _ body: (UnsafePointer<T>) throws -> Result
) rethrows -> Result
{
#if $BuiltinUnprotectedAddressOf
  return try body(UnsafePointer<T>(Builtin.unprotectedAddressOf(&value)))
#else
  return try body(UnsafePointer<T>(Builtin.addressof(&value)))
#endif
}

#if !$Embedded
extension String {
  /// Calls the given closure with a pointer to the contents of the string,
  /// represented as a null-terminated sequence of UTF-8 code units.
  ///
  /// The pointer passed as an argument to `body` is valid only during the
  /// execution of `withCString(_:)`. Do not store or return the pointer for
  /// later use.
  ///
  /// - Parameter body: A closure with a pointer parameter that points to a
  ///   null-terminated sequence of UTF-8 code units. If `body` has a return
  ///   value, that value is also used as the return value for the
  ///   `withCString(_:)` method. The pointer argument is valid only for the
  ///   duration of the method's execution.
  /// - Returns: The return value, if any, of the `body` closure parameter.
  @inlinable // fast-path: already C-string compatible
  public func withCString<Result>(
    _ body: (UnsafePointer<Int8>) throws -> Result
  ) rethrows -> Result {
    return try _guts.withCString(body)
  }
}
#endif

/// Takes in a value at +0 and performs a Builtin.copy upon it.
///
/// IMPLEMENTATION NOTES: During transparent inlining, Builtin.copy becomes the
/// explicit_copy_value instruction if we are inlining into a context where the
/// specialized type is loadable. If the transparent function is called in a
/// context where the inlined function specializes such that the specialized
/// type is still not loadable, the compiler aborts (a). Once we have opaque
/// values, this restriction will be lifted since after that address only types
/// at SILGen time will be loadable objects.
///
/// (a). This is implemented by requiring that Builtin.copy only be called
/// within a function marked with the semantic tag "lifetimemanagement.copy"
/// which conveniently is only the function we are defining here: _copy.
///
/// NOTE: We mark this _alwaysEmitIntoClient to ensure that we are not creating
/// new ABI that the stdlib must maintain if a user calls this ignoring the '_'
/// implying it is stdlib SPI.
@_alwaysEmitIntoClient
@inlinable
@_transparent
@_semantics("lifetimemanagement.copy")
public func _copy<T>(_ value: T) -> T {
  #if $BuiltinCopy
    Builtin.copy(value)
  #else
    value
  #endif
}
