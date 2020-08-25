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

#if SWIFT_ENABLE_REFLECTION

/// Dumps the given object's contents using its mirror to the specified output
/// stream.
///
/// - Parameters:
///   - value: The value to output to the `target` stream.
///   - target: The stream to use for writing the contents of `value`.
///   - name: A label to use when writing the contents of `value`. When `nil`
///     is passed, the label is omitted. The default is `nil`.
///   - indent: The number of spaces to use as an indent for each line of the
///     output. The default is `0`.
///   - maxDepth: The maximum depth to descend when writing the contents of a
///     value that has nested components. The default is `Int.max`.
///   - maxItems: The maximum number of elements for which to write the full
///     contents. The default is `Int.max`.
/// - Returns: The instance passed as `value`.
@discardableResult
@_semantics("optimize.sil.specialize.generic.never")
public func dump<T, TargetStream: TextOutputStream>(
  _ value: T,
  to target: inout TargetStream,
  name: String? = nil,
  indent: Int = 0,
  maxDepth: Int = .max,
  maxItems: Int = .max
) -> T {
  var maxItemCounter = maxItems
  var visitedItems = [ObjectIdentifier: Int]()
  target._lock()
  defer { target._unlock() }
  _dump_unlocked(
    value,
    to: &target,
    name: name,
    indent: indent,
    maxDepth: maxDepth,
    maxItemCounter: &maxItemCounter,
    visitedItems: &visitedItems)
  return value
}

/// Dumps the given object's contents using its mirror to standard output.
///
/// - Parameters:
///   - value: The value to output to the `target` stream.
///   - name: A label to use when writing the contents of `value`. When `nil`
///     is passed, the label is omitted. The default is `nil`.
///   - indent: The number of spaces to use as an indent for each line of the
///     output. The default is `0`.
///   - maxDepth: The maximum depth to descend when writing the contents of a
///     value that has nested components. The default is `Int.max`.
///   - maxItems: The maximum number of elements for which to write the full
///     contents. The default is `Int.max`.
/// - Returns: The instance passed as `value`.
@discardableResult
@_semantics("optimize.sil.specialize.generic.never")
public func dump<T>(
  _ value: T,
  name: String? = nil,
  indent: Int = 0,
  maxDepth: Int = .max,
  maxItems: Int = .max
) -> T {
  var stdoutStream = _Stdout()
  return dump(
    value,
    to: &stdoutStream,
    name: name,
    indent: indent,
    maxDepth: maxDepth,
    maxItems: maxItems)
}

/// Dump an object's contents. User code should use dump().
@_semantics("optimize.sil.specialize.generic.never")
internal func _dump_unlocked<TargetStream: TextOutputStream>(
  _ value: Any,
  to target: inout TargetStream,
  name: String?,
  indent: Int,
  maxDepth: Int,
  maxItemCounter: inout Int,
  visitedItems: inout [ObjectIdentifier: Int]
) {
  guard maxItemCounter > 0 else { return }
  maxItemCounter -= 1

  for _ in 0..<indent { target.write(" ") }

  let mirror = Mirror(reflecting: value)
  let count = mirror._children.count
  let bullet = count == 0    ? "-"
             : maxDepth <= 0 ? "▹" : "▿"
  target.write(bullet)
  target.write(" ")

  if let name = name {
    target.write(name)
    target.write(": ")
  }
  // This takes the place of the old mirror API's 'summary' property
  _dumpPrint_unlocked(value, mirror, &target)

  let id: ObjectIdentifier?
  if type(of: value) is AnyObject.Type {
    // Object is a class (but not an ObjC-bridged struct)
    id = ObjectIdentifier(_unsafeDowncastToAnyObject(fromAny: value))
  } else if let metatypeInstance = value as? Any.Type {
    // Object is a metatype
    id = ObjectIdentifier(metatypeInstance)
  } else {
    id = nil
  }
  if let theId = id {
    if let previous = visitedItems[theId] {
      target.write(" #")
      _print_unlocked(previous, &target)
      target.write("\n")
      return
    }
    let identifier = visitedItems.count
    visitedItems[theId] = identifier
    target.write(" #")
    _print_unlocked(identifier, &target)
  }

  target.write("\n")

  guard maxDepth > 0 else { return }

  if let superclassMirror = mirror.superclassMirror {
    _dumpSuperclass_unlocked(
      mirror: superclassMirror,
      to: &target,
      indent: indent + 2,
      maxDepth: maxDepth - 1,
      maxItemCounter: &maxItemCounter,
      visitedItems: &visitedItems)
  }

  var currentIndex = mirror._children.startIndex
  for i in 0..<count {
    if maxItemCounter <= 0 {
      for _ in 0..<(indent+4) {
        _print_unlocked(" ", &target)
      }
      let remainder = count - i
      target.write("(")
      _print_unlocked(remainder, &target)
      if i > 0 { target.write(" more") }
      if remainder == 1 {
        target.write(" child)\n")
      } else {
        target.write(" children)\n")
      }
      return
    }

    let (name, child) = mirror._children[currentIndex]
    mirror._children.formIndex(after: &currentIndex)
    _dump_unlocked(
      child,
      to: &target,
      name: name,
      indent: indent + 2,
      maxDepth: maxDepth - 1,
      maxItemCounter: &maxItemCounter,
      visitedItems: &visitedItems)
  }
}

/// Dump information about an object's superclass, given a mirror reflecting
/// that superclass.
@_semantics("optimize.sil.specialize.generic.never")
internal func _dumpSuperclass_unlocked<TargetStream: TextOutputStream>(
  mirror: Mirror,
  to target: inout TargetStream,
  indent: Int,
  maxDepth: Int,
  maxItemCounter: inout Int,
  visitedItems: inout [ObjectIdentifier: Int]
) {
  guard maxItemCounter > 0 else { return }
  maxItemCounter -= 1

  for _ in 0..<indent { target.write(" ") }

  let count = mirror._children.count
  let bullet = count == 0    ? "-"
             : maxDepth <= 0 ? "▹" : "▿"
  target.write(bullet)
  target.write(" super: ")
  _debugPrint_unlocked(mirror.subjectType, &target)
  target.write("\n")

  guard maxDepth > 0 else { return }

  if let superclassMirror = mirror.superclassMirror {
    _dumpSuperclass_unlocked(
      mirror: superclassMirror,
      to: &target,
      indent: indent + 2,
      maxDepth: maxDepth - 1,
      maxItemCounter: &maxItemCounter,
      visitedItems: &visitedItems)
  }

  var currentIndex = mirror._children.startIndex
  for i in 0..<count {
    if maxItemCounter <= 0 {
      for _ in 0..<(indent+4) {
        target.write(" ")
      }
      let remainder = count - i
      target.write("(")
      _print_unlocked(remainder, &target)
      if i > 0 { target.write(" more") }
      if remainder == 1 {
        target.write(" child)\n")
      } else {
        target.write(" children)\n")
      }
      return
    }

    let (name, child) = mirror._children[currentIndex]
    mirror._children.formIndex(after: &currentIndex)
    _dump_unlocked(
      child,
      to: &target,
      name: name,
      indent: indent + 2,
      maxDepth: maxDepth - 1,
      maxItemCounter: &maxItemCounter,
      visitedItems: &visitedItems)
  }
}

#endif // SWIFT_ENABLE_REFLECTION
