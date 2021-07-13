//===--- OpaqueIdentityFunctions.swift ------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

@_silgen_name("getPointer")
func _getPointer(_ x: OpaquePointer) -> OpaquePointer

public func _opaqueIdentity<T>(_ x: T) -> T {
  let ptr = UnsafeMutablePointer<T>.allocate(capacity: 1)
  ptr.initialize(to: x)
  let result =
    UnsafeMutablePointer<T>(_getPointer(OpaquePointer(ptr))).pointee
  ptr.deinitialize(count: 1)
  ptr.deallocate()
  return result
}

func _blackHolePtr<T>(_ x: UnsafePointer<T>) {
  _ = _getPointer(OpaquePointer(x))
}

public func _blackHole<T>(_ x: T) {
  var x = x
  _blackHolePtr(&x)
}

@inline(never)
public func getBool(_ x: Bool) -> Bool { return _opaqueIdentity(x) }

@inline(never)
public func getInt8(_ x: Int8) -> Int8 { return _opaqueIdentity(x) }

@inline(never)
public func getInt16(_ x: Int16) -> Int16 { return _opaqueIdentity(x) }

@inline(never)
public func getInt32(_ x: Int32) -> Int32 { return _opaqueIdentity(x) }

@inline(never)
public func getInt64(_ x: Int64) -> Int64 { return _opaqueIdentity(x) }

@inline(never)
public func getInt(_ x: Int) -> Int { return _opaqueIdentity(x) }

@inline(never)
public func getUInt8(_ x: UInt8) -> UInt8 { return _opaqueIdentity(x) }

@inline(never)
public func getUInt16(_ x: UInt16) -> UInt16 { return _opaqueIdentity(x) }

@inline(never)
public func getUInt32(_ x: UInt32) -> UInt32 { return _opaqueIdentity(x) }

@inline(never)
public func getUInt64(_ x: UInt64) -> UInt64 { return _opaqueIdentity(x) }

@inline(never)
public func getUInt(_ x: UInt) -> UInt { return _opaqueIdentity(x) }

#if !((os(macOS) || targetEnvironment(macCatalyst)) && arch(x86_64))
@available(macOS 11.0, iOS 14.0, watchOS 7.0, tvOS 14.0, *)
@inline(never)
public func getFloat16(_ x: Float16) -> Float16 { return _opaqueIdentity(x) }
#endif

@inline(never)
public func getFloat32(_ x: Float32) -> Float32 { return _opaqueIdentity(x) }

@inline(never)
public func getFloat64(_ x: Float64) -> Float64 { return _opaqueIdentity(x) }

#if !(os(Windows) || os(Android) || os(FreeBSD)) && (arch(i386) || arch(x86_64))
@inline(never)
public func getFloat80(_ x: Float80) -> Float80 { return _opaqueIdentity(x) }
#endif

public func getPointer(_ x: OpaquePointer) -> OpaquePointer {
  return _opaqueIdentity(x)
}
