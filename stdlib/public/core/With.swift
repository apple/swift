/// Makes a copy of `value` and invokes `transform` on it, then returns the modified value.
/// - Parameters:
///   - value: The value to modify.
///   - transform: The closure used to modify the value.
/// - Throws: An error, if the closure throws one.
/// - Returns: The modified value.
@inlinable // trivial implementation, generic
public func with<T>(_ value: T, transform: (inout T) throws -> Void) rethrows -> T {
    var copy = value
    try transform(&copy)
    return copy
}

/// Makes a copy of `value` and invokes `transform` on it, then returns the modified value.
/// - Parameters:
///   - value: The value to modify.
///   - transform: The closure used to modify the value.
/// - Throws: An error, if the closure throws one.
/// - Returns: The modified value.
@inlinable // trivial implementation, generic
public func with<T>(_ value: T, transform: (inout T) async throws -> Void) async rethrows -> T {
    var copy = value
    try await transform(&copy)
    return copy
}
