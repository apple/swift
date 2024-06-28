//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022-2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import IDEBridging
@_spi(Compiler) import SwiftIDEUtils
import SwiftSyntax
import swiftASTGen

// MARK: - Bridged type initializers

fileprivate extension BridgedCharSourceRange {
  init(from range: Range<AbsolutePosition>, in sourceFile: ExportedSourceFile) {
    self.init(
      start: BridgedSourceLoc(at: range.lowerBound, in: sourceFile.buffer),
      byteLength: UInt32(range.upperBound.utf8Offset - range.lowerBound.utf8Offset)
    )
  }
}

fileprivate extension BridgedCharSourceRangeVector {
  init(from ranges: some Sequence<Range<AbsolutePosition>>, in sourceFile: ExportedSourceFile) {
    self = .init()
    for range in ranges {
      self.append(BridgedCharSourceRange(from: range, in: sourceFile))
    }
  }
}

fileprivate extension BridgedResolvedLocVector {
  init(from resolvedLocs: some Sequence<DeclNameLocation>, in sourceFile: ExportedSourceFile) {
    self = .init()
    for resolvedLoc in resolvedLocs {
      self.append(BridgedResolvedLoc(from: resolvedLoc, in: sourceFile))
    }
  }
}

fileprivate extension IDEBridging.LabelRangeType {
  init(_ argumentType: DeclNameLocation.Arguments) {
    switch argumentType {
    case .noArguments: self = .None
    case .call: self = .CallArg
    case .parameters: self = .Param
    case .enumCaseParameters: self = .EnumCaseParam
    case .noncollapsibleParameters: self = .NoncollapsibleParam
    case .selector: self = .CompoundName
#if RESILIENT_SWIFT_SYNTAX
    @unknown default: fatalError()
#endif
    }
  }
}

extension BridgedResolvedLoc {
  init(from resolvedLoc: DeclNameLocation, in sourceFile: ExportedSourceFile) {
    let firstTrailingClosureIndex: UInt32
    if case .call(_, .some(let index)) = resolvedLoc.arguments {
      firstTrailingClosureIndex = UInt32(index)
    } else {
      firstTrailingClosureIndex = UInt32.max
    }
    let arguments: [DeclNameLocation.Argument]
    switch resolvedLoc.arguments {
    case .noArguments: arguments = []
    case .call(let arguments2, _): arguments = arguments2
    case .parameters(let arguments2): arguments = arguments2
    case .enumCaseParameters(let arguments2): arguments = arguments2
    case .noncollapsibleParameters(let arguments2): arguments = arguments2
    case .selector(let arguments2): arguments = arguments2
#if RESILIENT_SWIFT_SYNTAX
    @unknown default: fatalError()
#endif
    }
    self.init(
      range: BridgedCharSourceRange(from: resolvedLoc.baseNameRange, in: sourceFile),
      labelRanges: BridgedCharSourceRangeVector(from: arguments.map { $0.range }, in: sourceFile),
      firstTrailingLabel: firstTrailingClosureIndex,
      labelType: LabelRangeType(resolvedLoc.arguments),
      isActive: resolvedLoc.isActive,
      context: IDEBridging.ResolvedLocContext(resolvedLoc.context)
    )
  }
}

fileprivate extension IDEBridging.ResolvedLocContext {
  init(_ context: DeclNameLocation.Context) {
    switch context {
    case .default: self = .Default
    case .selector: self = .Selector
    case .comment: self = .Comment
    case .stringLiteral: self = .StringLiteral
#if RESILIENT_SWIFT_SYNTAX
    @unknown default: fatalError()
#endif
    }
  }
}

// MARK: - Run NameMatcher

@_cdecl("swift_SwiftIDEUtilsBridging_runNameMatcher")
public func runNameMatcher(
  sourceFilePtr: UnsafeRawPointer,
  locations: UnsafePointer<BridgedSourceLoc>,
  locationsCount: UInt
) -> UnsafeMutableRawPointer? {
  let sourceFile = sourceFilePtr.bindMemory(to: ExportedSourceFile.self, capacity: 1).pointee
  let locations = UnsafeBufferPointer(start: locations, count: Int(locationsCount))
  let positions: [AbsolutePosition] = locations.compactMap { sourceFile.position(of: $0) }
  let resolvedLocs = NameMatcher.resolve(baseNamePositions: positions, in: sourceFile.syntax)
  let bridged = BridgedResolvedLocVector(from: resolvedLocs, in: sourceFile)
  return bridged.getOpaqueValue()
}
