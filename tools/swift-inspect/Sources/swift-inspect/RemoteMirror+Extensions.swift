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

import SwiftRemoteMirror

extension swift_metadata_allocation_t {
  internal var tag: swift_metadata_allocation_tag_t { return self.Tag }
  internal var ptr: swift_reflection_ptr_t { return self.Ptr }
  internal var size: Int { return Int(self.Size) }
}

extension swift_metadata_allocation_t: Comparable {
  public static func == (lhs: Self, rhs: Self) -> Bool {
    lhs.ptr == rhs.ptr
  }

  public static func < (lhs: Self, rhs: Self) -> Bool {
    lhs.ptr < rhs.ptr
  }
}

extension SwiftReflectionContextRef {
  struct Error: Swift.Error, CustomStringConvertible {
    var description: String

    init(cString: UnsafePointer<CChar>) {
      description = String(cString: cString)
    }
  }
}

extension SwiftReflectionContextRef {
  typealias ConformanceIterationCallback = (swift_reflection_ptr_t, swift_reflection_ptr_t) -> Void
  typealias MetadataAllocationIterationCallback = (swift_metadata_allocation_t) -> Void
  typealias MetadataAllocationBacktraceIterationCallback = (swift_reflection_ptr_t, Int, UnsafePointer<swift_reflection_ptr_t>) -> Void

  internal var allocations: [swift_metadata_allocation_t] {
    get throws {
      var allocations: [swift_metadata_allocation_t] = []
      try iterateMetadataAllocations {
        allocations.append($0)
      }
      return allocations
    }
  }

  internal var allocationStacks: [swift_reflection_ptr_t:[swift_reflection_ptr_t]] {
    get throws {
      var stacks: [swift_reflection_ptr_t:[swift_reflection_ptr_t]] = [:]
      try iterateMetadataAllocationBacktraces { allocation, count, stack in
        stacks[allocation] =
            Array(UnsafeBufferPointer(start: stack, count: count))
      }
      return stacks
    }
  }

  internal func name(type: swift_reflection_ptr_t) -> String? {
    let typeref = swift_reflection_typeRefForMetadata(self, UInt(type))
    if typeref == 0 { return nil }

    guard let name = swift_reflection_copyDemangledNameForTypeRef(self, typeref) else {
      return nil
    }
    defer { free(name) }

    return String(cString: name)
  }

  internal func name(protocol: swift_reflection_ptr_t) -> String? {
    guard let name = swift_reflection_copyDemangledNameForProtocolDescriptor(self, `protocol`) else {
      return nil
    }
    defer { free(name) }

    return String(cString: name)
  }

  internal func name(allocation tag: swift_metadata_allocation_tag_t) -> String?  {
    return swift_reflection_metadataAllocationTagName(self, tag).map(String.init)
  }

  internal func isContiguousArray(_ array: swift_reflection_ptr_t) -> Bool {
    guard let name = name(type: array) else { return false }
    return name.hasPrefix("Swift._ContiguousArrayStorage")
  }

  internal func isArrayOfClass(_ array: swift_reflection_ptr_t) -> Bool {
    guard isContiguousArray(array) else { return false }

    let typeref = swift_reflection_typeRefForMetadata(self, UInt(array))
    if typeref == 0 { return false }

    let count = swift_reflection_genericArgumentCountOfTypeRef(typeref)
    guard count == 1 else { return false }

    let argument = swift_reflection_genericArgumentOfTypeRef(typeref, 0)
    if argument == 0 { return false }

    let info = swift_reflection_infoForTypeRef(self, argument)
    return info.Kind == SWIFT_STRONG_REFERENCE
  }

  internal func arrayCount(_ array: swift_reflection_ptr_t,
                           _ read: (swift_addr_t, Int) -> UnsafeRawPointer?) -> UInt? {
    // Array layout is: metadata, refCount, count
    let size = MemoryLayout<UInt>.stride * 3
    guard let pointer = read(swift_addr_t(array), size) else { return nil }
    let words = pointer.bindMemory(to: UInt.self, capacity: 3)
    return words[2]
  }

  internal func iterateConformanceCache(_ body: @escaping ConformanceIterationCallback) throws {
    var body = body
    if let error = withUnsafeMutablePointer(to: &body, {
      swift_reflection_iterateConformanceCache(self, {
        $2!.bindMemory(to: ConformanceIterationCallback.self, capacity: 1).pointee($0, $1)
      }, $0)
    }) {
      throw Error(cString: error)
    }
  }

  internal func iterateMetadataAllocations(_ body: @escaping MetadataAllocationIterationCallback) throws {
    var body = body
    if let error = withUnsafeMutablePointer(to: &body, {
      swift_reflection_iterateMetadataAllocations(self, {
        $1!.bindMemory(to: MetadataAllocationIterationCallback.self, capacity: 1).pointee($0)
      }, $0)
    }) {
      throw Error(cString: error)
    }
  }

  internal func iterateMetadataAllocationBacktraces(_ body: @escaping MetadataAllocationBacktraceIterationCallback) throws {
    var body = body
    if let error = withUnsafeMutablePointer(to: &body, {
      swift_reflection_iterateMetadataAllocationBacktraces(self, {
        $3!.bindMemory(to: MetadataAllocationBacktraceIterationCallback.self, capacity: 1).pointee($0, $1, $2!)
      }, $0)
    }) {
      throw Error(cString: error)
    }
  }
}
