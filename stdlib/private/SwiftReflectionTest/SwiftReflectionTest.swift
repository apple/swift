//===--- SwiftReflectionTest.swift ----------------------------------------===//
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
//
// This file provides infrastructure for introspecting type information in a
// remote swift executable by swift-reflection-test, using pipes and a
// request-response protocol to communicate with the test tool.
//
//===----------------------------------------------------------------------===//

let RequestInstanceKind = "k"
let RequestShouldUnwrapClassExistential = "u"
let RequestInstanceAddress = "i"
let RequestReflectionInfos = "r"
let RequestImages = "m"
let RequestReadBytes = "b"
let RequestSymbolAddress = "s"
let RequestStringLength = "l"
let RequestDone = "d"
let RequestPointerSize = "p"


#if os(macOS) || os(iOS) || os(watchOS) || os(tvOS)
import MachO
import Darwin

#if arch(x86_64) || arch(arm64)
typealias MachHeader = mach_header_64
#else
typealias MachHeader = mach_header
#endif

/// Get the location and size of a section in a binary.
///
/// - Parameter name: The name of the section
/// - Parameter imageHeader: A pointer to the Mach header describing the
///   image.
/// - Returns: A `Section` containing the address and size, or `nil` if there
///   is no section by the given name.
internal func getSectionInfo(_ name: String,
  _ imageHeader: UnsafePointer<MachHeader>) -> Section? {
  debugLog("BEGIN \(#function)"); defer { debugLog("END \(#function)") }
  var size: UInt = 0
  let address = getsectiondata(imageHeader, "__TEXT", name, &size)
  guard let nonNullAddress = address else { return nil }
  guard size != 0 else { return nil }
  return Section(startAddress: nonNullAddress, size: size)
}

/// Get the TEXT segment location and size for a loaded image.
///
/// - Parameter i: The index of the loaded image as reported by Dyld.
/// - Returns: The image name, address, and size.
internal func getAddressInfoForImage(atIndex i: UInt32) ->
        (name: String, address: UnsafeMutablePointer<UInt8>?, size: UInt) {
  debugLog("BEGIN \(#function)"); defer { debugLog("END \(#function)") }
  let header = unsafeBitCast(_dyld_get_image_header(i),
          to: UnsafePointer<MachHeader>.self)
  let name = String(validatingUTF8: _dyld_get_image_name(i)!)!
  var size: UInt = 0
  let address = getsegmentdata(header, "__TEXT", &size)
  return (name, address, size)
}

/// Send all loadedimages loaded in the current process.
internal func sendImages() {
  debugLog("BEGIN \(#function)"); defer { debugLog("END \(#function)") }
  let infos = (0..<getImageCount()).map(getAddressInfoForImage)

  debugLog("\(infos.count) reflection info bundles.")
  precondition(infos.count >= 1)
  sendValue(infos.count)
  for (name, address, size) in infos {
    debugLog("Sending info for \(name)")
    sendValue(address)
    sendValue(size)
  }
}

/// Get the Swift Reflection section locations for a loaded image.
///
/// An image of interest must have the following sections in the __TEXT
/// segment:
/// - __swift5_fieldmd
/// - __swift5_assocty
/// - __swift5_builtin
/// - __swift5_capture
/// - __swift5_typeref
/// - __swift5_reflstr (optional, may have been stripped out)
///
/// - Parameter i: The index of the loaded image as reported by Dyld.
/// - Returns: A `ReflectionInfo` containing the locations of all of the
///   needed sections, or `nil` if the image doesn't contain all of them.
internal func getReflectionInfoForImage(atIndex i: UInt32) -> ReflectionInfo? {
  debugLog("BEGIN \(#function)"); defer { debugLog("END \(#function)") }
  let header = unsafeBitCast(_dyld_get_image_header(i),
    to: UnsafePointer<MachHeader>.self)

  let imageName = _dyld_get_image_name(i)!
  let fieldmd = getSectionInfo("__swift5_fieldmd", header)
  let assocty = getSectionInfo("__swift5_assocty", header)
  let builtin = getSectionInfo("__swift5_builtin", header)
  let capture = getSectionInfo("__swift5_capture", header)
  let typeref = getSectionInfo("__swift5_typeref", header)
  let reflstr = getSectionInfo("__swift5_reflstr", header)
  return ReflectionInfo(imageName: String(validatingUTF8: imageName)!,
                        fieldmd: fieldmd,
                        assocty: assocty,
                        builtin: builtin,
                        capture: capture,
                        typeref: typeref,
                        reflstr: reflstr)
}

internal func getImageCount() -> UInt32 {
  return _dyld_image_count()
}

let rtldDefault = UnsafeMutableRawPointer(bitPattern: Int(-2))
#elseif !os(Windows)
import SwiftShims
#if canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#endif

let rtldDefault: UnsafeMutableRawPointer? = nil

#if INTERNAL_CHECKS_ENABLED
@_silgen_name("swift_getMetadataSection")
internal func _getMetadataSection(_ index: UInt) -> UnsafeRawPointer?

@_silgen_name("swift_getMetadataSectionCount")
internal func _getMetadataSectionCount() -> UInt

@_silgen_name("swift_getMetadataSectionName")
internal func _getMetadataSectionName(
  _ metadata_section: UnsafeRawPointer
) -> UnsafePointer<CChar>
#endif

extension Section {
  init(range: MetadataSectionRange) {
    self.startAddress = UnsafeRawPointer(bitPattern: range.start)!
    self.size = UInt(range.length)
  }
}

internal func getReflectionInfoForImage(atIndex i: UInt32) -> ReflectionInfo? {
#if INTERNAL_CHECKS_ENABLED
  return _getMetadataSection(UInt(i)).map { rawPointer in
    let name = _getMetadataSectionName(rawPointer)
    let metadataSection = rawPointer.bindMemory(to: MetadataSections.self, capacity: 1).pointee
    return ReflectionInfo(imageName: String(validatingUTF8: name)!,
            fieldmd: Section(range: metadataSection.swift5_fieldmd),
            assocty: Section(range: metadataSection.swift5_assocty),
            builtin: Section(range: metadataSection.swift5_builtin),
            capture: Section(range: metadataSection.swift5_capture),
            typeref: Section(range: metadataSection.swift5_typeref),
            reflstr: Section(range: metadataSection.swift5_reflstr))
  }
#else
  return nil
#endif
}

internal func getImageCount() -> UInt32 {
#if INTERNAL_CHECKS_ENABLED
  return UInt32(_getMetadataSectionCount())
#else
  return 0
#endif
}

internal func sendImages() {
  preconditionFailure("Should only be called in macOS!")
}


#else // os(Linux)
#error("SwiftReflectionTest does not currently support this OS.")
#endif

internal func debugLog(_ message: @autoclosure () -> String) {
#if DEBUG_LOG
  fputs("Child: \(message())\n", stderr)
  fflush(stderr)
#endif
}

public enum InstanceKind: UInt8 {
  case None
  case Object
  case Existential
  case ErrorExistential
  case Closure
  case Enum
  case EnumValue
  case AsyncTask
}

/// Represents a section in a loaded image in this process.
internal struct Section {
  /// The absolute start address of the section's data in this address space.
  let startAddress: UnsafeRawPointer

  /// The size of the section in bytes.
  let size: UInt
}

/// Holds the addresses and sizes of sections related to reflection.
internal struct ReflectionInfo : Sequence {
  /// The name of the loaded image.
  internal let imageName: String

  /// Reflection metadata sections.
  internal let fieldmd: Section?
  internal let assocty: Section?
  internal let builtin: Section?
  internal let capture: Section?
  internal let typeref: Section?
  internal let reflstr: Section?

  internal func makeIterator() -> AnyIterator<Section?> {
    return AnyIterator([
      fieldmd,
      assocty,
      builtin,
      capture,
      typeref,
      reflstr
    ].makeIterator())
  }
}

internal func sendBytes<T>(from address: UnsafePointer<T>, count: Int) {
  var source = UnsafeRawPointer(address)
  var bytesLeft = count
  debugLog("BEGIN \(#function)"); defer { debugLog("END \(#function)") }
  while bytesLeft > 0 {
    let bytesWritten = fwrite(source, 1, bytesLeft, stdout)
    fflush(stdout)
    guard bytesWritten > 0 else {
      fatalError("Couldn't write to parent pipe")
    }
    bytesLeft -= bytesWritten
    source = source.advanced(by: bytesWritten)
  }
}

/// Send the address of an object to the parent.
internal func sendAddress(of instance: AnyObject) {
  debugLog("BEGIN \(#function)")
  defer { debugLog("END \(#function)") }
  var address = Unmanaged.passUnretained(instance).toOpaque()
  sendBytes(from: &address, count: MemoryLayout<UInt>.size)
}

/// Send the `value`'s bits to the parent.
internal func sendValue<T>(_ value: T) {
  debugLog("BEGIN \(#function)"); defer { debugLog("END \(#function)") }
  var value = value
  sendBytes(from: &value, count: MemoryLayout<T>.size)
}

/// Read a word-sized unsigned integer from the parent.
internal func readUInt() -> UInt {
  debugLog("BEGIN \(#function)"); defer { debugLog("END \(#function)") }
  var value: UInt = 0
  fread(&value, MemoryLayout<UInt>.size, 1, stdin)
  return value
}

/// Send all known `ReflectionInfo`s for all images loaded in the current
/// process.
internal func sendReflectionInfos() {
  debugLog("BEGIN \(#function)"); defer { debugLog("END \(#function)") }
  let infos = (0..<getImageCount()).compactMap(getReflectionInfoForImage)

  var numInfos = infos.count
  debugLog("\(numInfos) reflection info bundles.")
  precondition(numInfos >= 1)
  sendBytes(from: &numInfos, count: MemoryLayout<UInt>.size)
  for info in infos {
    debugLog("Sending info for \(info.imageName)")
    for section in info {
      sendValue(section?.startAddress)
      sendValue(section?.size ?? 0)
    }
  }
}

internal func printErrnoAndExit() {
  debugLog("BEGIN \(#function)"); defer { debugLog("END \(#function)") }
  let errorCString = strerror(errno)!
  let message = String(validatingUTF8: errorCString)! + "\n"
  let bytes = Array(message.utf8)
  fwrite(bytes, 1, bytes.count, stderr)
  fflush(stderr)
  exit(EXIT_FAILURE)
}

/// Retrieve the address and count from the parent and send the bytes back.
internal func sendBytes() {
  debugLog("BEGIN \(#function)"); defer { debugLog("END \(#function)") }
  let address = readUInt()
  let count = Int(readUInt())
  debugLog("Parent requested \(count) bytes from \(address)")
  var totalBytesWritten = 0
  var pointer = UnsafeMutableRawPointer(bitPattern: address)
  while totalBytesWritten < count {
    let bytesWritten = Int(fwrite(pointer, 1, Int(count), stdout))
    fflush(stdout)
    if bytesWritten == 0 {
      printErrnoAndExit()
    }
    totalBytesWritten += bytesWritten
    pointer = pointer?.advanced(by: bytesWritten)
  }
}

/// Send the address of a symbol loaded in this process.
internal func sendSymbolAddress() {
  debugLog("BEGIN \(#function)"); defer { debugLog("END \(#function)") }
  let name = readLine()!
  name.withCString {
    let symbol = dlsym(rtldDefault, $0)
    let symbolAddress = unsafeBitCast(symbol, to: UInt.self)
    sendValue(symbolAddress)
  }
}

/// Send the length of a string to the parent.
internal func sendStringLength() {
  debugLog("BEGIN \(#function)"); defer { debugLog("END \(#function)") }
  let address = readUInt()
  let cString = UnsafePointer<CChar>(bitPattern: address)!
  var count = 0
  while cString[count] != CChar(0) {
    count = count + 1
  }
  sendValue(count)
}

/// Send the size of this architecture's pointer type.
internal func sendPointerSize() {
  debugLog("BEGIN \(#function)"); defer { debugLog("END \(#function)") }
  let pointerSize = UInt8(MemoryLayout<UnsafeRawPointer>.size)
  sendValue(pointerSize)
}

/// Hold an `instance` and wait for the parent to query for information.
///
/// This is the main "run loop" of the test harness.
///
/// The parent will necessarily need to:
/// - Get the addresses of any swift dylibs that are loaded, where applicable.
/// - Get the address of the `instance`
/// - Get the pointer size of this process, which affects assumptions about the
///   the layout of runtime structures with pointer-sized fields.
/// - Read raw bytes out of this process's address space.
///
/// The parent sends a Done message to indicate that it's done
/// looking at this instance. It will continue to ask for instances,
/// so call doneReflecting() when you don't have any more instances.
internal func reflect(instanceAddress: UInt, 
                      kind: InstanceKind, 
                      shouldUnwrapClassExistential: Bool = false) {
  while let command = readLine(strippingNewline: true) {
    switch command {
    case RequestInstanceKind:
      sendValue(kind.rawValue)
    case RequestShouldUnwrapClassExistential:
      sendValue(shouldUnwrapClassExistential)
    case RequestInstanceAddress:
      sendValue(instanceAddress)
    case RequestReflectionInfos:
      sendReflectionInfos()
    case RequestImages:
      sendImages()
    case RequestReadBytes:
      sendBytes()
    case RequestSymbolAddress:
      sendSymbolAddress()
    case RequestStringLength:
      sendStringLength()
    case RequestPointerSize:
      sendPointerSize()
    case RequestDone:
      return
    default:
      fatalError("Unknown request received: '\(Array(command.utf8))'!")
    }
  }
}

/// Reflect a class instance.
///
/// This reflects the stored properties of the immediate class.
/// The superclass is not (yet?) visited.
public func reflect(object: AnyObject) {
  defer { _fixLifetime(object) }
  let address = Unmanaged.passUnretained(object).toOpaque()
  let addressValue = UInt(bitPattern: address)
  reflect(instanceAddress: addressValue, kind: .Object)
}

/// Reflect any type at all by boxing it into an existential container (an `Any`)
///
/// Given a class, this will reflect the reference value, and not the contents
/// of the instance. Use `reflect(object:)` for that.
///
/// This function serves to exercise the projectExistential function of the
/// SwiftRemoteMirror API.
///
/// It tests the three conditions of existential layout:
///
/// ## Class existentials
///
/// For example, a `MyClass as Any`:
/// ```
/// [Pointer to class instance]
/// [Witness table 1]
/// [Witness table 2]
/// ...
/// [Witness table n]
/// ```
///
/// ## Existentials whose contained type fits in the 3-word buffer
///
/// For example, a `(1, 2) as Any`:
/// ```
/// [Tuple element 1: Int]
/// [Tuple element 2: Int]
/// [-Empty_]
/// [Metadata Pointer]
/// [Witness table 1]
/// [Witness table 2]
/// ...
/// [Witness table n]
/// ```
///
/// ## Existentials whose contained type has to be allocated into a
///    heap buffer.
///
/// For example, a `LargeStruct<T> as Any`:
/// ```
/// [Pointer to unmanaged heap container] --> [Large struct]
/// [-Empty-]
/// [-Empty-]
/// [Metadata Pointer]
/// [Witness table 1]
/// [Witness table 2]
/// ...
/// [Witness table n]
/// ```
///
/// The test doesn't care about the witness tables - we only care
/// about what's in the buffer, so we always put these values into
/// an Any existential.
///
/// If shouldUnwrapClassExistential is set to true, this exercises 
/// projectExistentialAndUnwrapClass instead of projectExistential.
public func reflect<T>(any: T, kind: InstanceKind = .Existential, 
    shouldUnwrapClassExistential: Bool = false) {
  let any: Any = any
  let anyPointer = UnsafeMutablePointer<Any>.allocate(capacity: MemoryLayout<Any>.size)
  anyPointer.initialize(to: any)
  let anyPointerValue = UInt(bitPattern: anyPointer)
  reflect(instanceAddress: anyPointerValue, 
          kind: kind, 
          shouldUnwrapClassExistential: shouldUnwrapClassExistential)
  anyPointer.deallocate()
}

// Reflect an `Error`, a.k.a. an "error existential".
//
// These are always boxed on the heap, with the following layout:
//
// - Word 0: Metadata Pointer
// - Word 1: 2x 32-bit reference counts
//
// If Objective-C interop is available, an Error is also an
// `NSError`, and so has:
//
// - Word 2: code (NSInteger)
// - Word 3: domain (NSString *)
// - Word 4: userInfo (NSDictionary *)
//
// Then, always follow:
//
// - Word 2 or 5: Instance type metadata pointer
// - Word 3 or 6: Instance witness table for conforming
//   to `Swift.Error`.
//
// Following that is the instance that conforms to `Error`,
// rounding up to its alignment.
public func reflect<T: Error>(error: T) {
  let error: Error = error
  let errorPointerValue = unsafeBitCast(error, to: UInt.self)
  reflect(instanceAddress: errorPointerValue, kind: .ErrorExistential)
}

// Like reflect<T: Error>(error: T), but calls projectExistentialAndUnwrapClass 
// instead of projectExistential and adds an extra level of indirection, which is
// what projectExistentialAndUnwrapClass expects.
public func reflectUnwrappingClassExistential<T: Error>(error: T) {
  let error: Error = error
  let errorPointerValue = unsafeBitCast(error, to: UInt.self)
  let anyPointer = UnsafeMutablePointer<Any>.allocate(capacity: MemoryLayout<Any>.size)
  anyPointer.initialize(to: errorPointerValue)
  let anyPointerValue = UInt(bitPattern: anyPointer)
  reflect(instanceAddress: anyPointerValue, 
          kind: .ErrorExistential, 
          shouldUnwrapClassExistential: true)
  anyPointer.deallocate()
}

// Reflect an `Enum`
//
// These are handled like existentials, but
// the test driver verifies a different set of data.
public func reflect<T>(enum value: T) {
  reflect(any: value, kind: .Enum)
}

public func reflect<T>(enumValue value: T) {
  reflect(any: value, kind: .EnumValue)
}

/// Wraps a thick function with arity 0.
struct ThickFunction0 {
  var function: () -> Void
}

/// Wraps a thick function with arity 1.
struct ThickFunction1 {
  var function: (Int) -> Void
}

/// Wraps a thick function with arity 2.
struct ThickFunction2 {
  var function: (Int, String) -> Void
}

/// Wraps a thick function with arity 3.
struct ThickFunction3 {
  var function: (Int, String, AnyObject?) -> Void
}

struct ThickFunctionParts {
  var function: UnsafeRawPointer
  var context: Optional<UnsafeRawPointer>
}

/// Reflect a closure context. The given function must be a Swift-native
/// @convention(thick) function value.
public func reflect(function: @escaping () -> Void) {
  let fn = UnsafeMutablePointer<ThickFunction0>.allocate(
    capacity: MemoryLayout<ThickFunction0>.size)
  fn.initialize(to: ThickFunction0(function: function))

  let contextPointer = fn.withMemoryRebound(
    to: ThickFunctionParts.self, capacity: 1) {
      UInt(bitPattern: $0.pointee.context)
  }

  reflect(instanceAddress: contextPointer, kind: .Object)

  fn.deallocate()
}

/// Reflect a closure context. The given function must be a Swift-native
/// @convention(thick) function value.
public func reflect(function: @escaping (Int) -> Void) {
  let fn =
  UnsafeMutablePointer<ThickFunction1>.allocate(
    capacity: MemoryLayout<ThickFunction1>.size)
  fn.initialize(to: ThickFunction1(function: function))

  let contextPointer = fn.withMemoryRebound(
    to: ThickFunctionParts.self, capacity: 1) {
      UInt(bitPattern: $0.pointee.context)
  }

  reflect(instanceAddress: contextPointer, kind: .Object)

  fn.deallocate()
}

/// Reflect a closure context. The given function must be a Swift-native
/// @convention(thick) function value.
public func reflect(function: @escaping (Int, String) -> Void) {
  let fn = UnsafeMutablePointer<ThickFunction2>.allocate(
      capacity: MemoryLayout<ThickFunction2>.size)
  fn.initialize(to: ThickFunction2(function: function))

  let contextPointer = fn.withMemoryRebound(
    to: ThickFunctionParts.self, capacity: 1) {
      UInt(bitPattern: $0.pointee.context)
  }

  reflect(instanceAddress: contextPointer, kind: .Object)

  fn.deallocate()
}

/// Reflect a closure context. The given function must be a Swift-native
/// @convention(thick) function value.
public func reflect(function: @escaping (Int, String, AnyObject?) -> Void) {
  let fn = UnsafeMutablePointer<ThickFunction3>.allocate(
      capacity: MemoryLayout<ThickFunction3>.size)
  fn.initialize(to: ThickFunction3(function: function))

  let contextPointer = fn.withMemoryRebound(
    to: ThickFunctionParts.self, capacity: 1) {
      UInt(bitPattern: $0.pointee.context)
  }

  reflect(instanceAddress: contextPointer, kind: .Object)

  fn.deallocate()
}


/// Reflect an AsyncTask.
public func reflect(asyncTask: UInt) {
  reflect(instanceAddress: asyncTask, kind: .AsyncTask)
}

/// Call this function to indicate to the parent that there are
/// no more instances to look at.
public func doneReflecting() {
  reflect(instanceAddress: UInt(InstanceKind.None.rawValue), kind: .None)
}

/* Example usage

public protocol P {
  associatedtype Index
  var startIndex: Index { get }
}

public struct Thing : P {
  public let startIndex = 1010
}

public enum E<T: P> {
  case B(T)
  case C(T.Index)
}

public class A<T: P> : P {
  public let x: T?
  public let y: T.Index
  public let b = true
  public let t = (1, 1.0)
  private let type: NSObject.Type
  public let startIndex = 1010
  public init(x: T) {
    self.x = x
    self.y = x.startIndex
    self.type = NSObject.self
  }
}
let instance = A(x: A(x: Thing()))

reflect(A(x: Thing()))
*/
