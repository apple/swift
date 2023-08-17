//===--- SourceLoc.swift - SourceLoc bridging utilities ------------------===//
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

import ASTBridging

public struct SourceLoc {
  /// Points into a source file.
  let locationInFile: UnsafePointer<UInt8>

  public init?(locationInFile: UnsafePointer<UInt8>?) {
    guard let locationInFile = locationInFile else {
      return nil
    }
    self.locationInFile = locationInFile
  }

  public init?(bridged: swift.SourceLoc) {
    guard bridged.isValid() else {
      return nil
    }
#if $NewCxxMethodSafetyHeuristics
    self.locationInFile = bridged.getOpaquePointerValue().assumingMemoryBound(to: UInt8.self)
#else
    self.locationInFile = bridged.__getOpaquePointerValueUnsafe().assumingMemoryBound(to: UInt8.self)
#endif
  }

  public var bridged: swift.SourceLoc {
    .init(llvm.SMLoc.getFromPointer(locationInFile))
  }
}

extension SourceLoc {
  public func advanced(by n: Int) -> SourceLoc {
    SourceLoc(locationInFile: locationInFile.advanced(by: n))!
  }
}

extension Optional where Wrapped == SourceLoc {
  public var bridged: swift.SourceLoc {
    self?.bridged ?? .init()
  }
}

public struct CharSourceRange {
  private let start: SourceLoc
  private let byteLength: UInt32

  public init(start: SourceLoc, byteLength: UInt32) {
    self.start = start
    self.byteLength = byteLength
  }

  public init?(bridged: swift.CharSourceRange) {
#if $NewCxxMethodSafetyHeuristics
    guard let start = SourceLoc(bridged: bridged.getStart()) else {
      return nil
    }
#else
    guard let start = SourceLoc(bridged: bridged.__getStartUnsafe()) else {
      return nil
    }
#endif
    self.init(start: start, byteLength: bridged.getByteLength())
  }

  public var bridged: swift.CharSourceRange {
    .init(start.bridged, byteLength)
  }
}

extension Optional where Wrapped == CharSourceRange {
  public var bridged: swift.CharSourceRange {
    self?.bridged ?? .init(.init(), 0)
  }
}
