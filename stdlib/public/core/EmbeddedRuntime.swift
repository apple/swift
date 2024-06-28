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

import SwiftShims

/// Class object and class metadata structures

public struct ClassMetadata {
  var superclassMetadata: UnsafeMutablePointer<ClassMetadata>?

  // There is no way to express the actual calling convention on this
  // function (swiftcc with 'self') currently, so let's use UnsafeRawPointer
  // and a helper function in C (_swift_embedded_invoke_heap_object_destroy).
  var destroy: UnsafeRawPointer

  // There is no way to express the actual calling convention on this
  // function (swiftcc with 'self') currently, so let's use UnsafeRawPointer
  // and a helper function in C (_swift_embedded_invoke_heap_object_optional_ivardestroyer).
  var ivarDestroyer: UnsafeRawPointer?
}

public struct HeapObject {
  // There is no way to express the custom ptrauth signature on the metadata
  // field, so let's use UnsafeRawPointer and a helper function in C instead
  // (_swift_embedded_set_heap_object_metadata_pointer).
  var metadata: UnsafeRawPointer

  // TODO: This is just an initial support for strong refcounting only. We need
  // to think about supporting (or banning) weak and/or unowned references.
  var refcount: Int

#if _pointerBitWidth(_64)
  static let doNotFreeBit = Int(bitPattern: 0x8000_0000_0000_0000)
  static let refcountMask = Int(bitPattern: 0x7fff_ffff_ffff_ffff)
#else
  static let doNotFreeBit = Int(bitPattern: 0x8000_0000)
  static let refcountMask = Int(bitPattern: 0x7fff_ffff)
#endif

  // Note: The immortalRefCount value of -1 is also hard-coded in IRGen in `irgen::emitConstantObject`.
  static let immortalRefCount = -1

#if _pointerBitWidth(_64)
  static let immortalObjectPointerBit = UInt(0x8000_0000_0000_0000)
#endif

#if _pointerBitWidth(_64)
  static let bridgeObjectToPlainObjectMask = UInt(0x8fff_ffff_ffff_fff8)
#else
  static let bridgeObjectToPlainObjectMask = UInt(0xffff_ffff)
#endif
}



/// Forward declarations of C functions

@_extern(c, "posix_memalign")
func posix_memalign(_: UnsafeMutablePointer<UnsafeMutableRawPointer?>, _: Int, _: Int) -> CInt

@_extern(c, "free")
func free(_ p: Builtin.RawPointer)



/// Allocations

func alignedAlloc(size: Int, alignment: Int) -> UnsafeMutableRawPointer? {
  let alignment = max(alignment, MemoryLayout<UnsafeRawPointer>.size)
  var r: UnsafeMutableRawPointer? = nil
  _ = posix_memalign(&r, alignment, size)
  return r
}

@_cdecl("swift_slowAlloc")
public func swift_slowAlloc(_ size: Int, _ alignMask: Int) -> UnsafeMutableRawPointer? {
  let alignment: Int
  if alignMask == -1 {
    alignment = _swift_MinAllocationAlignment
  } else {
    alignment = alignMask + 1
  }
  return alignedAlloc(size: size, alignment: alignment)
}

@_cdecl("swift_slowDealloc")
public func swift_slowDealloc(_ ptr: UnsafeMutableRawPointer, _ size: Int, _ alignMask: Int) {
  free(ptr._rawValue)
}

@_cdecl("swift_allocObject")
public func swift_allocObject(metadata: Builtin.RawPointer, requiredSize: Int, requiredAlignmentMask: Int) -> Builtin.RawPointer {
  return swift_allocObject(metadata: UnsafeMutablePointer<ClassMetadata>(metadata), requiredSize: requiredSize, requiredAlignmentMask: requiredAlignmentMask)._rawValue
}

func swift_allocObject(metadata: UnsafeMutablePointer<ClassMetadata>, requiredSize: Int, requiredAlignmentMask: Int) -> UnsafeMutablePointer<HeapObject> {
  let p = swift_slowAlloc(requiredSize, requiredAlignmentMask)!
  let object = p.assumingMemoryBound(to: HeapObject.self)
  _swift_embedded_set_heap_object_metadata_pointer(object, metadata)
  object.pointee.refcount = 1
  return object
}

@_cdecl("swift_deallocObject")
public func swift_deallocObject(object: Builtin.RawPointer, allocatedSize: Int, allocatedAlignMask: Int) {
  swift_deallocObject(object: UnsafeMutablePointer<HeapObject>(object), allocatedSize: allocatedSize, allocatedAlignMask: allocatedAlignMask)
}

func swift_deallocObject(object: UnsafeMutablePointer<HeapObject>, allocatedSize: Int, allocatedAlignMask: Int) {
  free(object._rawValue)
}

@_cdecl("swift_deallocClassInstance")
public func swift_deallocClassInstance(object: Builtin.RawPointer, allocatedSize: Int, allocatedAlignMask: Int) {
  swift_deallocClassInstance(object: UnsafeMutablePointer<HeapObject>(object), allocatedSize: allocatedSize, allocatedAlignMask: allocatedAlignMask)
}

func swift_deallocClassInstance(object: UnsafeMutablePointer<HeapObject>, allocatedSize: Int, allocatedAlignMask: Int) {
  if (object.pointee.refcount & HeapObject.doNotFreeBit) != 0 {
    return
  }

  free(object._rawValue)
}

@_cdecl("swift_deallocPartialClassInstance")
public func swift_deallocPartialClassInstance(object: Builtin.RawPointer, metadata: Builtin.RawPointer, allocatedSize: Int, allocatedAlignMask: Int) {
  swift_deallocPartialClassInstance(object: UnsafeMutablePointer<HeapObject>(object), metadata: UnsafeMutablePointer<ClassMetadata>(metadata), allocatedSize: allocatedSize, allocatedAlignMask: allocatedAlignMask)
}

func swift_deallocPartialClassInstance(object: UnsafeMutablePointer<HeapObject>, metadata: UnsafeMutablePointer<ClassMetadata>, allocatedSize: Int, allocatedAlignMask: Int) {
  var classMetadata = _swift_embedded_get_heap_object_metadata_pointer(object).assumingMemoryBound(to: ClassMetadata.self)
  while classMetadata != metadata {
    _swift_embedded_invoke_heap_object_optional_ivardestroyer(object, classMetadata)
    guard let superclassMetadata = classMetadata.pointee.superclassMetadata else { break }
    classMetadata = superclassMetadata
  }
}

@_cdecl("swift_initStaticObject")
public func swift_initStaticObject(metadata: Builtin.RawPointer, object: Builtin.RawPointer) -> Builtin.RawPointer {
  return swift_initStaticObject(metadata: UnsafeMutablePointer<ClassMetadata>(metadata), object: UnsafeMutablePointer<HeapObject>(object))._rawValue
}

func swift_initStaticObject(metadata: UnsafeMutablePointer<ClassMetadata>, object: UnsafeMutablePointer<HeapObject>) -> UnsafeMutablePointer<HeapObject> {
  _swift_embedded_set_heap_object_metadata_pointer(object, metadata)
  object.pointee.refcount = HeapObject.immortalRefCount
  return object
}

@_cdecl("swift_initStackObject")
public func swift_initStackObject(metadata: Builtin.RawPointer, object: Builtin.RawPointer) -> Builtin.RawPointer {
  return swift_initStackObject(metadata: UnsafeMutablePointer<ClassMetadata>(metadata), object: UnsafeMutablePointer<HeapObject>(object))._rawValue
}

func swift_initStackObject(metadata: UnsafeMutablePointer<ClassMetadata>, object: UnsafeMutablePointer<HeapObject>) -> UnsafeMutablePointer<HeapObject> {
  _swift_embedded_set_heap_object_metadata_pointer(object, metadata)
  object.pointee.refcount = 1 | HeapObject.doNotFreeBit
  return object
}



/// Refcounting

func isValidPointerForNativeRetain(object: Builtin.RawPointer) -> Bool {
  let objectBits = UInt(Builtin.ptrtoint_Word(object))
  if objectBits == 0 { return false }

  #if _pointerBitWidth(_64)
  if (objectBits & HeapObject.immortalObjectPointerBit) != 0 { return false }
  #endif
  
  return true
}

@_cdecl("swift_setDeallocating")
public func swift_setDeallocating(object: Builtin.RawPointer) {
}

@_cdecl("swift_dynamicCastClass")
public func swift_dynamicCastClass(object: UnsafeMutableRawPointer, targetMetadata: UnsafeRawPointer) -> UnsafeMutableRawPointer? {
  let sourceObj = object.assumingMemoryBound(to: HeapObject.self)
  var type = _swift_embedded_get_heap_object_metadata_pointer(sourceObj).assumingMemoryBound(to: ClassMetadata.self)
  let targetType = targetMetadata.assumingMemoryBound(to: ClassMetadata.self)
  while type != targetType {
    guard let superType = type.pointee.superclassMetadata else {
      return nil
    }
    type = UnsafeMutablePointer(superType)
  }
  return object
}

@_cdecl("swift_isUniquelyReferenced_native")
public func swift_isUniquelyReferenced_native(object: Builtin.RawPointer) -> Bool {
  if !isValidPointerForNativeRetain(object: object) { return false }

  return swift_isUniquelyReferenced_nonNull_native(object: UnsafeMutablePointer<HeapObject>(object))
}

@_cdecl("swift_isUniquelyReferenced_nonNull_native")
public func swift_isUniquelyReferenced_nonNull_native(object: Builtin.RawPointer) -> Bool {
  return swift_isUniquelyReferenced_nonNull_native(object: UnsafeMutablePointer<HeapObject>(object))
}

func swift_isUniquelyReferenced_nonNull_native(object: UnsafeMutablePointer<HeapObject>) -> Bool {
  let refcount = refcountPointer(for: object)
  return loadAcquire(refcount) == 1
}

@_cdecl("swift_retain")
public func swift_retain(object: Builtin.RawPointer) -> Builtin.RawPointer {
  if !isValidPointerForNativeRetain(object: object) { return object }

  let o = UnsafeMutablePointer<HeapObject>(object)
  return swift_retain_n_(object: o, n: 1)._rawValue
}

// Cannot use UnsafeMutablePointer<HeapObject>? directly in the function argument or return value as it causes IRGen crashes
@_cdecl("swift_retain_n")
public func swift_retain_n(object: Builtin.RawPointer, n: UInt32) -> Builtin.RawPointer {
  if !isValidPointerForNativeRetain(object: object) { return object }

  let o = UnsafeMutablePointer<HeapObject>(object)
  return swift_retain_n_(object: o, n: n)._rawValue
}

func swift_retain_n_(object: UnsafeMutablePointer<HeapObject>, n: UInt32) -> UnsafeMutablePointer<HeapObject> {
  let refcount = refcountPointer(for: object)
  if loadRelaxed(refcount) == HeapObject.immortalRefCount {
    return object
  }

  addRelaxed(refcount, n: Int(n))

  return object
}

@_cdecl("swift_bridgeObjectRetain")
public func swift_bridgeObjectRetain(object: Builtin.RawPointer) -> Builtin.RawPointer {
  return swift_bridgeObjectRetain_n(object: object, n: 1)
}

@_cdecl("swift_bridgeObjectRetain_n")
public func swift_bridgeObjectRetain_n(object: Builtin.RawPointer, n: UInt32) -> Builtin.RawPointer {
  let objectBits = UInt(Builtin.ptrtoint_Word(object))
  let untaggedObject = Builtin.inttoptr_Word((objectBits & HeapObject.bridgeObjectToPlainObjectMask)._builtinWordValue)
  return swift_retain_n(object: untaggedObject, n: n)
}

@_cdecl("swift_release")
public func swift_release(object: Builtin.RawPointer) {
  if !isValidPointerForNativeRetain(object: object) { return }

  let o = UnsafeMutablePointer<HeapObject>(object)
  swift_release_n_(object: o, n: 1)
}

@_cdecl("swift_release_n")
public func swift_release_n(object: Builtin.RawPointer, n: UInt32) {
  if !isValidPointerForNativeRetain(object: object) { return }

  let o = UnsafeMutablePointer<HeapObject>(object)
  swift_release_n_(object: o, n: n)
}

func swift_release_n_(object: UnsafeMutablePointer<HeapObject>?, n: UInt32) {
  guard let object else {
    return
  }

  let refcount = refcountPointer(for: object)
  if loadRelaxed(refcount) == HeapObject.immortalRefCount {
    return
  }

  let resultingRefcount = subFetchAcquireRelease(refcount, n: Int(n)) & HeapObject.refcountMask
  if resultingRefcount == 0 {
    _swift_embedded_invoke_heap_object_destroy(object)
  } else if resultingRefcount < 0 {
    fatalError("negative refcount")
  }
}

@_cdecl("swift_bridgeObjectRelease")
public func swift_bridgeObjectRelease(object: Builtin.RawPointer) {
  swift_bridgeObjectRelease_n(object: object, n: 1)
}

@_cdecl("swift_bridgeObjectRelease_n")
public func swift_bridgeObjectRelease_n(object: Builtin.RawPointer, n: UInt32) {
  let objectBits = UInt(Builtin.ptrtoint_Word(object))
  let untaggedObject = Builtin.inttoptr_Word((objectBits & HeapObject.bridgeObjectToPlainObjectMask)._builtinWordValue)
  swift_release_n(object: untaggedObject, n: n)
}



/// Refcount helpers

fileprivate func refcountPointer(for object: UnsafeMutablePointer<HeapObject>) -> UnsafeMutablePointer<Int> {
  // TODO: This should use MemoryLayout<HeapObject>.offset(to: \.refcount) but we don't have KeyPaths yet
  return UnsafeMutablePointer<Int>(UnsafeRawPointer(object).advanced(by: MemoryLayout<Int>.size)._rawValue)
}

fileprivate func loadRelaxed(_ atomic: UnsafeMutablePointer<Int>) -> Int {
  Int(Builtin.atomicload_monotonic_Word(atomic._rawValue))
}

fileprivate func loadAcquire(_ atomic: UnsafeMutablePointer<Int>) -> Int {
  Int(Builtin.atomicload_acquire_Word(atomic._rawValue))
}

fileprivate func subFetchAcquireRelease(_ atomic: UnsafeMutablePointer<Int>, n: Int) -> Int {
  let oldValue = Int(Builtin.atomicrmw_sub_acqrel_Word(atomic._rawValue, n._builtinWordValue))
  return oldValue - n
}

fileprivate func addRelaxed(_ atomic: UnsafeMutablePointer<Int>, n: Int) {
  _ = Builtin.atomicrmw_add_monotonic_Word(atomic._rawValue, n._builtinWordValue)
}

fileprivate func compareExchangeRelaxed(_ atomic: UnsafeMutablePointer<Int>, expectedOldValue: Int, desiredNewValue: Int) -> Bool {
  let (_, won) = Builtin.cmpxchg_monotonic_monotonic_Word(atomic._rawValue, expectedOldValue._builtinWordValue, desiredNewValue._builtinWordValue)
  return Bool(won)
}

fileprivate func storeRelease(_ atomic: UnsafeMutablePointer<Int>, newValue: Int) {
  Builtin.atomicstore_release_Word(atomic._rawValue, newValue._builtinWordValue)
}


/// Exclusivity checking

@_cdecl("swift_beginAccess")
public func swift_beginAccess(pointer: UnsafeMutableRawPointer, buffer: UnsafeMutableRawPointer, flags: UInt, pc: UnsafeMutableRawPointer) {
  // TODO: Add actual exclusivity checking.
}

@_cdecl("swift_endAccess")
public func swift_endAccess(buffer: UnsafeMutableRawPointer) {
  // TODO: Add actual exclusivity checking.
}



// Once

@_cdecl("swift_once")
public func swift_once(predicate: UnsafeMutablePointer<Int>, fn: (@convention(c) (UnsafeMutableRawPointer)->()), context: UnsafeMutableRawPointer) {
  let checkedLoadAcquire = { predicate in
    let value = loadAcquire(predicate)
    assert(value == -1 || value == 0 || value == 1)
    return value
  }

  if checkedLoadAcquire(predicate) < 0 { return }

  let won = compareExchangeRelaxed(predicate, expectedOldValue: 0, desiredNewValue: 1)
  if won {
    fn(context)
    storeRelease(predicate, newValue: -1)
    return
  }

  // TODO: This should really use an OS provided lock
  while checkedLoadAcquire(predicate) >= 0 {
    // spin
  }
}



// Misc

@_cdecl("swift_deletedMethodError")
public func swift_deletedMethodError() -> Never {
  Builtin.int_trap()
}

@_silgen_name("swift_willThrow") // This is actually expected to be swiftcc (@_silgen_name and not @_cdecl).
public func swift_willThrow() throws {
}

/// Called when a typed error will be thrown.
@_silgen_name("swift_willThrowTyped")
public func _willThrowTyped<E: Error>(_ error: E) {
}

@_extern(c, "arc4random_buf")
func arc4random_buf(buf: UnsafeMutableRawPointer, nbytes: Int)

public func swift_stdlib_random(_ buf: UnsafeMutableRawPointer, _ nbytes: Int) {
  arc4random_buf(buf: buf, nbytes: nbytes)
}

@_cdecl("swift_clearSensitive")
@inline(never)
public func swift_clearSensitive(buf: UnsafeMutableRawPointer, nbytes: Int) {
  // TODO: use memset_s if available
  // Though, it shouldn't make too much difference because the `@inline(never)` should prevent
  // the optimizer from removing the loop below.
  let bytePtr = buf.assumingMemoryBound(to: UInt8.self)
  for i in 0..<nbytes {
    bytePtr[i] = 0
  }
}

@usableFromInline
func _embeddedReportFatalError(prefix: StaticString, message: StaticString) {
  print(prefix, terminator: "")
  if message.utf8CodeUnitCount > 0 { print(": ", terminator: "") }
  print(message)
}

@usableFromInline
func _embeddedReportFatalErrorInFile(prefix: StaticString, message: StaticString, file: StaticString, line: UInt) {
  print(file, terminator: ":")
  print(line, terminator: ": ")
  print(prefix, terminator: "")
  if message.utf8CodeUnitCount > 0 { print(": ", terminator: "") }
  print(message)
}
