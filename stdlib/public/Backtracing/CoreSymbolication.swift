//===--- CoreSymbolication.swift - Shims for CoreSymbolication ------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// CoreSymbolication is a private framework, which makes it tricky to link
// with from here and also means there are no headers on customer builds.
//
//===----------------------------------------------------------------------===//

#if os(iOS) || os(macOS) || os(tvOS) || os(watchOS)

import Swift

@_implementationOnly import OS.Libc
@_implementationOnly import OS.Darwin

// .. Dynamic binding ..........................................................

private let coreFoundationPath =
  "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"

private let coreFoundationHandle = dlopen(coreFoundationPath, RTLD_LAZY)!

private let coreSymbolicationPath =
  "/System/Library/PrivateFrameworks/CoreSymbolication.framework/CoreSymbolication"
private let coreSymbolicationHandle = dlopen(coreSymbolicationPath, RTLD_LAZY)!

private let crashReporterSupportPath =
  "/System/Library/PrivateFrameworks/CrashReporterSupport.framework/CrashReporterSupport"

private let crashReporterSupportHandle
  = dlopen(crashReporterSupportPath, RTLD_LAZY)!

private func symbol<T>(_ handle: UnsafeMutableRawPointer, _ name: String) -> T {
  guard let result = dlsym(handle, name) else {
    fatalError("Unable to look up \(name) in CoreSymbolication")
  }
  return unsafeBitCast(result, to: T.self)
}

private enum Sym {
  // CRCopySanitizedPath
  static let CRCopySanitizedPath: @convention(c) (CFString, CFIndex) -> CFString =
    symbol(crashReporterSupportHandle, "CRCopySanitizedPath")

  // Base functionality
  static let CSRetain: @convention(c) (CSTypeRef) -> CSTypeRef =
    symbol(coreSymbolicationHandle, "CSRetain")
  static let CSRelease: @convention(c) (CSTypeRef) -> () =
    symbol(coreSymbolicationHandle, "CSRelease")
  static let CSEqual: @convention(c) (CSTypeRef, CSTypeRef) -> CBool =
    symbol(coreSymbolicationHandle, "CSEqual")
  static let CSIsNull: @convention(c) (CSTypeRef) -> CBool =
    symbol(coreSymbolicationHandle, "CSIsNull")

  // CSSymbolicator
  static let CSSymbolicatorCreateWithBinaryImageList:
    @convention(c) (UnsafeMutablePointer<CSBinaryImageInformation>,
                    UInt32, UInt32, CSNotificationBlock?) -> CSSymbolicatorRef =
    symbol(coreSymbolicationHandle, "CSSymbolicatorCreateWithBinaryImageList")

  static let CSSymbolicatorGetSymbolOwnerWithAddressAtTime:
    @convention(c) (CSSymbolicatorRef, vm_address_t,
                    CSMachineTime) -> CSSymbolOwnerRef =
    symbol(coreSymbolicationHandle, "CSSymbolicatorGetSymbolOwnerWithAddressAtTime")
  static let CSSymbolicatorForeachSymbolOwnerAtTime:
    @convention(c) (CSSymbolicatorRef, CSMachineTime, @convention(block) (CSSymbolOwnerRef) -> Void) -> UInt =
      symbol(coreSymbolicationHandle, "CSSymbolicatorForeachSymbolOwnerAtTime")

  // CSSymbolOwner
  static let CSSymbolOwnerGetName:
    @convention(c) (CSSymbolOwnerRef) -> UnsafePointer<CChar>? =
    symbol(coreSymbolicationHandle, "CSSymbolOwnerGetName")
  static let CSSymbolOwnerGetSymbolWithAddress:
    @convention(c) (CSSymbolOwnerRef, vm_address_t) -> CSSymbolRef =
    symbol(coreSymbolicationHandle, "CSSymbolOwnerGetSymbolWithAddress")
  static let CSSymbolOwnerGetSourceInfoWithAddress:
    @convention(c) (CSSymbolOwnerRef, vm_address_t) -> CSSourceInfoRef =
    symbol(coreSymbolicationHandle, "CSSymbolOwnerGetSourceInfoWithAddress")
  static let CSSymbolOwnerForEachStackFrameAtAddress:
    @convention(c) (CSSymbolOwnerRef, vm_address_t, CSStackFrameIterator) -> UInt =
    symbol(coreSymbolicationHandle, "CSSymbolOwnerForEachStackFrameAtAddress")
  static let CSSymbolOwnerGetBaseAddress:
    @convention(c) (CSSymbolOwnerRef) -> vm_address_t =
    symbol(coreSymbolicationHandle, "CSSymbolOwnerGetBaseAddress")

  // CSSymbol
  static let CSSymbolGetRange:
    @convention(c) (CSSymbolRef) -> CSRange =
    symbol(coreSymbolicationHandle, "CSSymbolGetRange")
  static let CSSymbolGetName:
    @convention(c) (CSSymbolRef) -> UnsafePointer<CChar>? =
    symbol(coreSymbolicationHandle, "CSSymbolGetName")
  static let CSSymbolGetMangledName:
    @convention(c) (CSSymbolRef) -> UnsafePointer<CChar>? =
    symbol(coreSymbolicationHandle, "CSSymbolGetMangledName")

  // CSSourceInfo
  static let CSSourceInfoGetPath:
    @convention(c) (CSSourceInfoRef) -> UnsafePointer<CChar>? =
    symbol(coreSymbolicationHandle, "CSSourceInfoGetPath")
  static let CSSourceInfoGetLineNumber:
    @convention(c) (CSSourceInfoRef) -> UInt32 =
    symbol(coreSymbolicationHandle, "CSSourceInfoGetLineNumber")
  static let CSSourceInfoGetColumn:
    @convention(c) (CSSourceInfoRef) -> UInt32 =
    symbol(coreSymbolicationHandle, "CSSourceInfoGetColumn")

  // CFString
  static let CFStringCreateWithBytes:
    @convention(c) (CFAllocator?, UnsafeRawPointer?, CFIndex,
                    CFStringEncoding, Bool) -> CFString? =
    symbol(coreFoundationHandle, "CFStringCreateWithBytes")
  static let CFStringGetLength:
    @convention(c) (CFString) -> CFIndex =
    symbol(coreFoundationHandle, "CFStringGetLength")
  static let CFStringGetCStringPtr:
    @convention(c) (CFString, CFStringEncoding) -> UnsafePointer<CChar>? =
    symbol(coreFoundationHandle, "CFStringGetCStringPtr")
  static let CFStringGetBytes:
    @convention(c) (CFString, CFRange, CFStringEncoding, UInt8, Bool,
                    UnsafeMutableRawPointer?, CFIndex,
                    UnsafeMutablePointer<CFIndex>?) -> CFIndex =
    symbol(coreFoundationHandle, "CFStringGetBytes")
}

// .. Core Foundation miscellany ...............................................

internal func CFRangeMake(_ location: CFIndex, _ length: CFIndex) -> CFRange {
  return CFRange(location: location, length: length)
}

internal func CFStringCreateWithBytes(_ allocator: CFAllocator?,
                                      _ bytes: UnsafeRawPointer?,
                                      _ length: CFIndex,
                                      _ encoding: CFStringEncoding,
                                      _ isExternalRepresentation: Bool)
  -> CFString? {
  return Sym.CFStringCreateWithBytes(allocator,
                                     bytes,
                                     length,
                                     encoding,
                                     isExternalRepresentation)
}

internal func CFStringGetLength(_ s: CFString) -> CFIndex {
  return Sym.CFStringGetLength(s)
}

internal func CFStringGetCStringPtr(_ s: CFString,
                                    _ encoding: CFStringEncoding)
  -> UnsafePointer<CChar>? {
  return Sym.CFStringGetCStringPtr(s, encoding)
}

internal func CFStringGetBytes(_ s: CFString,
                               _ range: CFRange,
                               _ encoding: CFStringEncoding,
                               _ lossByte: UInt8,
                               _ isExternalRepresentation: Bool,
                               _ buffer: UnsafeMutableRawPointer?,
                               _ maxBufLen: CFIndex,
                               _ usedBufLen: UnsafeMutablePointer<CFIndex>?)
  -> CFIndex {
  return Sym.CFStringGetBytes(s, range, encoding, lossByte,
                              isExternalRepresentation, buffer, maxBufLen,
                              usedBufLen)
}

// .. Crash Reporter support ...................................................

// We can't import swiftFoundation here, so there's no automatic bridging for
// CFString.  As a result, we need to do the dance manually.

private func toCFString(_ s: String) -> CFString! {
  var s = s
  return s.withUTF8 {
    return CFStringCreateWithBytes(nil,
                                   $0.baseAddress,
                                   $0.count,
                                   CFStringBuiltInEncodings.UTF8.rawValue,
                                   false)
  }
}

private func fromCFString(_ cf: CFString) -> String {
  let length = CFStringGetLength(cf)
  if length == 0 {
    return ""
  }

  if let ptr = CFStringGetCStringPtr(cf,
                                     CFStringBuiltInEncodings.ASCII.rawValue) {
    return String(decoding: UnsafeRawBufferPointer(start: ptr, count: length),
                  as: UTF8.self)
  } else {
    var byteLen = CFIndex(0)

    _ = CFStringGetBytes(cf,
                         CFRangeMake(0, length),
                         CFStringBuiltInEncodings.UTF8.rawValue,
                         0,
                         false,
                         nil,
                         0,
                         &byteLen)

    let buffer = UnsafeMutableBufferPointer<UInt8>.allocate(capacity: byteLen)
    defer {
      buffer.deallocate()
    }

    _ = CFStringGetBytes(cf, CFRangeMake(0, length),
                         CFStringBuiltInEncodings.UTF8.rawValue,
                         0, false, buffer.baseAddress, buffer.count, nil)

    return String(decoding: buffer, as: UTF8.self)
  }
}

func CRCopySanitizedPath(_ path: String, _ options: Int) -> String {
  return fromCFString(Sym.CRCopySanitizedPath(toCFString(path), CFIndex(options)))
}

// .. Base functionality .......................................................

func CSRetain(_ obj: CSTypeRef) -> CSTypeRef {
  return Sym.CSRetain(obj)
}

func CSRelease(_ obj: CSTypeRef) {
  Sym.CSRelease(obj)
}

func CSEqual(_ a: CSTypeRef, _ b: CSTypeRef) -> Bool {
  return Sym.CSEqual(a, b)
}

func CSIsNull(_ obj: CSTypeRef) -> Bool {
  return Sym.CSIsNull(obj)
}

// .. CSSymbolicator ...........................................................

let kCSSymbolicatorDisallowDaemonCommunication = UInt32(0x00000800)

struct BinaryRelocationInformation {
  var base: vm_address_t
  var extent: vm_address_t
  var name: String
}

struct BinaryImageInformation {
  var base: vm_address_t
  var extent: vm_address_t
  var uuid: CFUUIDBytes
  var arch: CSArchitecture
  var path: String
  var relocations: [BinaryRelocationInformation]
  var flags: UInt32
}

func CSSymbolicatorCreateWithBinaryImageList(
  _ imageInfo: [BinaryImageInformation],
  _ flags: UInt32,
  _ notificationBlock: CSNotificationBlock?) -> CSSymbolicatorRef {

  // Convert the Swifty types above to suitable input for the C API
  var pathBuf: [UInt8] = []
  let imageList = UnsafeMutableBufferPointer<CSBinaryImageInformation>.allocate(capacity: imageInfo.count)
  defer {
    imageList.deallocate()
  }

  var totalRelocations = 0
  for image in imageInfo {
    totalRelocations += image.relocations.count

    pathBuf.insert(contentsOf: image.path.utf8, at: pathBuf.count)
    pathBuf.append(0)
  }

  let relocationList = UnsafeMutableBufferPointer<CSBinaryRelocationInformation>.allocate(capacity: totalRelocations)
  defer {
    relocationList.deallocate()
  }

  return pathBuf.withUnsafeBufferPointer {
    $0.withMemoryRebound(to: CChar.self) { pathData in
      var pathPtr = pathData.baseAddress!
      var relocationPtr = relocationList.baseAddress!

      for (n, image) in imageInfo.enumerated() {
        imageList[n].base = image.base
        imageList[n].extent = image.extent
        imageList[n].uuid = image.uuid
        imageList[n].arch = image.arch
        imageList[n].path = pathPtr
        imageList[n].relocations = relocationPtr
        imageList[n].relocationCount = UInt32(image.relocations.count)
        imageList[n].flags = image.flags

        pathPtr += strlen(pathPtr) + 1

        for relocation in image.relocations {
          relocationPtr.pointee.base = relocation.base
          relocationPtr.pointee.extent = relocation.extent
          withUnsafeMutablePointer(to: &relocationPtr.pointee.name) {
            $0.withMemoryRebound(to: CChar.self, capacity: 17) { buf in
              var utf8Iterator = relocation.name.utf8.makeIterator()
              var ndx = 0
              while let ch = utf8Iterator.next(), ndx < 16 {
                buf[ndx] = CChar(bitPattern: ch)
                ndx += 1
              }
              buf[ndx] = 0
            }
          }

          relocationPtr += 1
        }
      }

      return Sym.CSSymbolicatorCreateWithBinaryImageList(
        imageList.baseAddress!,
        UInt32(imageList.count),
        flags,
        notificationBlock
      )
    }
  }
}

func CSSymbolicatorGetSymbolOwnerWithAddressAtTime(
  _ symbolicator: CSSymbolicatorRef,
  _ addr: vm_address_t,
  _ time: CSMachineTime
) -> CSSymbolOwnerRef {
  return Sym.CSSymbolicatorGetSymbolOwnerWithAddressAtTime(symbolicator,
                                                           addr, time)
}

func CSSymbolicatorForeachSymbolOwnerAtTime(
  _ symbolicator: CSSymbolicatorRef,
  _ time: CSMachineTime,
  _ symbolIterator: (CSSymbolOwnerRef) -> Void
  ) ->  UInt {
      return Sym.CSSymbolicatorForeachSymbolOwnerAtTime(symbolicator, time,
                                                        symbolIterator)
}

// .. CSSymbolOwner ............................................................

func CSSymbolOwnerGetName(_ sym: CSTypeRef) -> String? {
  Sym.CSSymbolOwnerGetName(sym)
    .map(String.init(cString:))
}

func CSSymbolOwnerGetSymbolWithAddress(
  _ owner: CSSymbolOwnerRef,
  _ address: vm_address_t
) -> CSSymbolRef {
  return Sym.CSSymbolOwnerGetSymbolWithAddress(owner, address)
}

func CSSymbolOwnerGetSourceInfoWithAddress(
  _ owner: CSSymbolOwnerRef,
  _ address: vm_address_t
) -> CSSourceInfoRef {
  return Sym.CSSymbolOwnerGetSourceInfoWithAddress(owner, address)
}

func CSSymbolOwnerForEachStackFrameAtAddress(
  _ owner: CSSymbolOwnerRef,
  _ address: vm_address_t,
  _ iterator: CSStackFrameIterator
) -> UInt {
  return Sym.CSSymbolOwnerForEachStackFrameAtAddress(owner, address, iterator)
}

func CSSymbolOwnerGetBaseAddress(
  _ owner: CSSymbolOwnerRef
) -> vm_address_t {
  return Sym.CSSymbolOwnerGetBaseAddress(owner)
}

// .. CSSymbol .................................................................

func CSSymbolGetRange(_ symbol: CSSymbolRef) -> CSRange {
  return Sym.CSSymbolGetRange(symbol)
}

func CSSymbolGetName(_ symbol: CSSymbolRef) -> String? {
  return Sym.CSSymbolGetName(symbol).map{ String(cString: $0) }
}

func CSSymbolGetMangledName(_ symbol: CSSymbolRef) -> String? {
  return Sym.CSSymbolGetMangledName(symbol).map{ String(cString: $0) }
}

// .. CSSourceInfo .............................................................

func CSSourceInfoGetPath(_ sourceInfo: CSSourceInfoRef) -> String? {
  return Sym.CSSourceInfoGetPath(sourceInfo).map{ String(cString: $0) }
}

func CSSourceInfoGetLineNumber(_ sourceInfo: CSSourceInfoRef) -> UInt32 {
  return Sym.CSSourceInfoGetLineNumber(sourceInfo)
}

func CSSourceInfoGetColumn(_ sourceInfo: CSSourceInfoRef) -> UInt32 {
  return Sym.CSSourceInfoGetColumn(sourceInfo)
}

#endif // os(Darwin)
