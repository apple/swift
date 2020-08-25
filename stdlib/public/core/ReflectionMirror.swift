//===----------------------------------------------------------------------===//
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

#if SWIFT_ENABLE_REFLECTION

@_silgen_name("swift_isClassType")
internal func _isClassType(_: Any.Type) -> Bool

@_silgen_name("swift_getMetadataKind")
internal func _metadataKind(_: Any.Type) -> UInt

@_silgen_name("swift_reflectionMirror_normalizedType")
internal func _getNormalizedType<T>(_: T, type: Any.Type) -> Any.Type

@_silgen_name("swift_reflectionMirror_count")
internal func _getChildCount<T>(_: T, type: Any.Type) -> Int

@_silgen_name("swift_reflectionMirror_recursiveCount")
internal func _getRecursiveChildCount(_: Any.Type) -> Int

@_silgen_name("swift_reflectionMirror_recursiveChildMetadata")
internal func _getChildMetadata(
  _: Any.Type,
  index: Int,
  outName: UnsafeMutablePointer<UnsafePointer<CChar>?>,
  outFreeFunc: UnsafeMutablePointer<NameFreeFunc?>
) -> Any.Type

@_silgen_name("swift_reflectionMirror_recursiveChildOffset")
internal func _getChildOffset(
  _: Any.Type,
  index: Int
) -> Int

internal typealias NameFreeFunc = @convention(c) (UnsafePointer<CChar>?) -> Void

@_silgen_name("swift_reflectionMirror_subscript")
internal func _getChild<T>(
  of: T,
  type: Any.Type,
  index: Int,
  outName: UnsafeMutablePointer<UnsafePointer<CChar>?>,
  outFreeFunc: UnsafeMutablePointer<NameFreeFunc?>
) -> Any

// Returns 'c' (class), 'e' (enum), 's' (struct), 't' (tuple), or '\0' (none)
@_silgen_name("swift_reflectionMirror_displayStyle")
internal func _getDisplayStyle<T>(_: T) -> CChar

internal func getChild<T>(of value: T, type: Any.Type, index: Int) -> (label: String?, value: Any) {
  var nameC: UnsafePointer<CChar>? = nil
  var freeFunc: NameFreeFunc? = nil
  
  let value = _getChild(of: value, type: type, index: index, outName: &nameC, outFreeFunc: &freeFunc)
  
  let name = nameC.flatMap({ String(validatingUTF8: $0) })
  freeFunc?(nameC)
  return (name, value)
}

#if _runtime(_ObjC)
@_silgen_name("swift_reflectionMirror_quickLookObject")
internal func _getQuickLookObject<T>(_: T) -> AnyObject?

@_silgen_name("_swift_stdlib_NSObject_isKindOfClass")
internal func _isImpl(_ object: AnyObject, kindOf: UnsafePointer<CChar>) -> Bool

internal func _is(_ object: AnyObject, kindOf `class`: String) -> Bool {
  return `class`.withCString {
    return _isImpl(object, kindOf: $0)
  }
}

internal func _getClassPlaygroundQuickLook(
  _ object: AnyObject
) -> _PlaygroundQuickLook? {
  if _is(object, kindOf: "NSNumber") {
    let number: _NSNumber = unsafeBitCast(object, to: _NSNumber.self)
    switch UInt8(number.objCType[0]) {
    case UInt8(ascii: "d"):
      return .double(number.doubleValue)
    case UInt8(ascii: "f"):
      return .float(number.floatValue)
    case UInt8(ascii: "Q"):
      return .uInt(number.unsignedLongLongValue)
    default:
      return .int(number.longLongValue)
    }
  }
  
  if _is(object, kindOf: "NSAttributedString") {
    return .attributedString(object)
  }
  
  if _is(object, kindOf: "NSImage") ||
     _is(object, kindOf: "UIImage") ||
     _is(object, kindOf: "NSImageView") ||
     _is(object, kindOf: "UIImageView") ||
     _is(object, kindOf: "CIImage") ||
     _is(object, kindOf: "NSBitmapImageRep") {
    return .image(object)
  }
  
  if _is(object, kindOf: "NSColor") ||
     _is(object, kindOf: "UIColor") {
    return .color(object)
  }
  
  if _is(object, kindOf: "NSBezierPath") ||
     _is(object, kindOf: "UIBezierPath") {
    return .bezierPath(object)
  }
  
  if _is(object, kindOf: "NSString") {
    return .text(_forceBridgeFromObjectiveC(object, String.self))
  }

  return .none
}
#endif

extension Mirror {
  internal struct ReflectedChildren: RandomAccessCollection {
    let subject: Any
    let subjectType: Any.Type
    var startIndex: Int { 0 }
    var endIndex: Int { _getChildCount(subject, type: subjectType) }
    subscript(index: Int) -> Child {
      getChild(of: subject, type: subjectType, index: index)
    }
  }

  internal init(
    internalReflecting subject: Any,
    subjectType: Any.Type? = nil,
    customAncestor: Mirror? = nil
  ) {
    let subjectType = subjectType ?? _getNormalizedType(subject, type: type(of: subject))
    
    self._children = _Children(
      ReflectedChildren(subject: subject, subjectType: subjectType))
    
    self._makeSuperclassMirror = {
      guard let subjectClass = subjectType as? AnyClass,
            let superclass = _getSuperclass(subjectClass) else {
        return nil
      }
      
      // Handle custom ancestors. If we've hit the custom ancestor's subject type,
      // or descendants are suppressed, return it. Otherwise continue reflecting.
      if let customAncestor = customAncestor {
        if superclass == customAncestor.subjectType {
          return customAncestor
        }
        if customAncestor._defaultDescendantRepresentation == .suppressed {
          return customAncestor
        }
      }
      return Mirror(internalReflecting: subject,
                    subjectType: superclass,
                    customAncestor: customAncestor)
    }
    
    let rawDisplayStyle = _getDisplayStyle(subject)
    switch UnicodeScalar(Int(rawDisplayStyle)) {
    case "c": self.displayStyle = .class
    case "e": self.displayStyle = .enum
    case "s": self.displayStyle = .struct
    case "t": self.displayStyle = .tuple
    case "\0": self.displayStyle = nil
    default: preconditionFailure("Unknown raw display style '\(rawDisplayStyle)'")
    }
    
    self.subjectType = subjectType
    self._defaultDescendantRepresentation = .generated
  }
  
  internal static func quickLookObject(_ subject: Any) -> _PlaygroundQuickLook? {
#if _runtime(_ObjC)
    let object = _getQuickLookObject(subject)
    return object.flatMap(_getClassPlaygroundQuickLook)
#else
    return nil
#endif
  }
}

/// Options for calling `_forEachField(of:options:body:)`.
@available(macOS 10.15.4, iOS 13.4, tvOS 13.4, watchOS 6.2, *)
@_spi(Reflection)
public struct _EachFieldOptions: OptionSet {
  public var rawValue: UInt32

  public init(rawValue: UInt32) {
    self.rawValue = rawValue
  }

  /// Require the top-level type to be a class.
  ///
  /// If this is not set, the top-level type is required to be a struct or
  /// tuple.
  public static var classType = _EachFieldOptions(rawValue: 1 << 0)

  /// Ignore fields that can't be introspected.
  ///
  /// If not set, the presence of things that can't be introspected causes
  /// the function to immediately return `false`.
  public static var ignoreUnknown = _EachFieldOptions(rawValue: 1 << 1)
}

/// The metadata "kind" for a type.
@available(macOS 10.15.4, iOS 13.4, tvOS 13.4, watchOS 6.2, *)
@_spi(Reflection)
public enum _MetadataKind: UInt {
  // With "flags":
  // runtimePrivate = 0x100
  // nonHeap = 0x200
  // nonType = 0x400
  
  case `class` = 0
  case `struct` = 0x200     // 0 | nonHeap
  case `enum` = 0x201       // 1 | nonHeap
  case optional = 0x202     // 2 | nonHeap
  case foreignClass = 0x203 // 3 | nonHeap
  case opaque = 0x300       // 0 | runtimePrivate | nonHeap
  case tuple = 0x301        // 1 | runtimePrivate | nonHeap
  case function = 0x302     // 2 | runtimePrivate | nonHeap
  case existential = 0x303  // 3 | runtimePrivate | nonHeap
  case metatype = 0x304     // 4 | runtimePrivate | nonHeap
  case objcClassWrapper = 0x305     // 5 | runtimePrivate | nonHeap
  case existentialMetatype = 0x306  // 6 | runtimePrivate | nonHeap
  case heapLocalVariable = 0x400    // 0 | nonType
  case heapGenericLocalVariable = 0x500 // 0 | nonType | runtimePrivate
  case errorObject = 0x501  // 1 | nonType | runtimePrivate
  case unknown = 0xffff
  
  init(_ type: Any.Type) {
    let v = _metadataKind(type)
    if let result = _MetadataKind(rawValue: v) {
      self = result
    } else {
      self = .unknown
    }
  }
}

/// Calls the given closure on every field of the specified type.
///
/// If `body` returns `false` for any field, no additional fields are visited.
///
/// - Parameters:
///   - type: The type to inspect.
///   - options: Options to use when reflecting over `type`.
///   - body: A closure to call with information about each field in `type`.
///     The parameters to `body` are a pointer to a C string holding the name
///     of the field, the offset of the field in bytes, the type of the field,
///     and the `_MetadataKind` of the field's type.
/// - Returns: `true` if every invocation of `body` returns `true`; otherwise,
///   `false`.
@available(macOS 10.15.4, iOS 13.4, tvOS 13.4, watchOS 6.2, *)
@discardableResult
@_spi(Reflection)
public func _forEachField(
  of type: Any.Type,
  options: _EachFieldOptions = [],
  body: (UnsafePointer<CChar>, Int, Any.Type, _MetadataKind) -> Bool
) -> Bool {
  // Require class type iff `.classType` is included as an option
  if _isClassType(type) != options.contains(.classType) {
    return false
  }

  let childCount = _getRecursiveChildCount(type)
  for i in 0..<childCount {
    let offset = _getChildOffset(type, index: i)

    var nameC: UnsafePointer<CChar>? = nil
    var freeFunc: NameFreeFunc? = nil
    let childType = _getChildMetadata(
      type, index: i, outName: &nameC, outFreeFunc: &freeFunc)
    defer { freeFunc?(nameC) }
    let kind = _MetadataKind(childType)

    if !body(nameC!, offset, childType, kind) {
      return false
    }
  }

  return true
}

#endif
