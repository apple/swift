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

import SwiftShims

internal func _isClassType(_ type: Any.Type) -> Bool {
  // a thick metatype is represented with a pointer metadata structure,
  // so this unsafeBitCast is a safe operation here.
  return swift_isClassType(unsafeBitCast(type, to: UnsafeRawPointer.self))
}

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
  fieldMetadata: UnsafeMutablePointer<_FieldReflectionMetadata>
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
  internal init(internalReflecting subject: Any,
              subjectType: Any.Type? = nil,
              customAncestor: Mirror? = nil)
  {
    let subjectType = subjectType ?? _getNormalizedType(subject, type: type(of: subject))
    
    let childCount = _getChildCount(subject, type: subjectType)
    let children = (0 ..< childCount).lazy.map({
      getChild(of: subject, type: subjectType, index: $0)
    })
    self.children = Children(children)
    
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
@available(SwiftStdlib 5.2, *)
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
@available(SwiftStdlib 5.2, *)
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
@available(SwiftStdlib 5.2, *)
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

    var field = _FieldReflectionMetadata()
    let childType = _getChildMetadata(type, index: i, fieldMetadata: &field)
    defer { field.freeFunc?(field.name) }
    let kind = _MetadataKind(childType)

    if let name = field.name {
      if !body(name, offset, childType, kind) {
        return false
      }
    } else {
      if !body("", offset, childType, kind) {
        return false
      }
    }
  }

  return true
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
///     of the field and an erased keypath for it.
/// - Returns: `true` if every invocation of `body` returns `true`; otherwise,
///   `false`.
@available(SwiftStdlib 5.4, *)
@discardableResult
@_spi(Reflection)
public func _forEachFieldWithKeyPath<Root>(
  of type: Root.Type,
  options: _EachFieldOptions = [],
  body: (UnsafePointer<CChar>, PartialKeyPath<Root>) -> Bool
) -> Bool {
  // Class types not supported because the metadata does not have
  // enough information to construct computed properties.
  if _isClassType(type) || options.contains(.classType) {
    return false
  }
  let ignoreUnknown = options.contains(.ignoreUnknown)

  let childCount = _getRecursiveChildCount(type)
  for i in 0..<childCount {
    let offset = _getChildOffset(type, index: i)

    var field = _FieldReflectionMetadata()
    let childType = _getChildMetadata(type, index: i, fieldMetadata: &field)
    defer { field.freeFunc?(field.name) }
    let kind = _MetadataKind(childType)
    let supportedType: Bool
    switch kind {
      case .struct, .class, .optional, .existential,
          .existentialMetatype, .tuple, .enum:
        supportedType = true
      default:
        supportedType = false
    }
    if !supportedType || !field.isStrong {
      if !ignoreUnknown { return false }
      continue;
    }
    func keyPathType<Leaf>(for: Leaf.Type) -> PartialKeyPath<Root>.Type {
      if field.isVar { return WritableKeyPath<Root, Leaf>.self }
      return KeyPath<Root, Leaf>.self
    }
    let resultSize = MemoryLayout<Int32>.size + MemoryLayout<Int>.size
    let partialKeyPath = _openExistential(childType, do: keyPathType)
       ._create(capacityInBytes: resultSize) {
      var destBuilder = KeyPathBuffer.Builder($0)
      destBuilder.pushHeader(KeyPathBuffer.Header(
        size: resultSize - MemoryLayout<Int>.size,
        trivial: true,
        hasReferencePrefix: false
      ))
      let component = RawKeyPathComponent(
           header: RawKeyPathComponent.Header(stored: .struct,
                                              mutable: field.isVar,
                                              inlineOffset: UInt32(offset)),
           body: UnsafeRawBufferPointer(start: nil, count: 0))
      component.clone(
        into: &destBuilder.buffer,
        endOfReferencePrefix: false)
    }

    if let name = field.name {
      if !body(name, partialKeyPath) {
        return false
      }
    } else {
      if !body("", partialKeyPath) {
        return false
      }
    }
  }

  return true
}

#endif
