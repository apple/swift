//===--- Set.swift - sets for basic blocks, values and instructions -------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

import SIL
import OptimizerBridging

protocol IntrusiveSet : CustomStringConvertible, NoReflectionChildren {
  associatedtype Element

  init(_ context: some Context)
  mutating func insert(_ element: Element) -> Bool
  mutating func erase(_ element: Element)
  func contains(_ element: Element) -> Bool
  mutating func deinitialize()
}

/// A set of basic blocks.
///
/// This is an extremely efficient implementation which does not need memory
/// allocations or hash lookups.
///
/// This type should be a move-only type, but unfortunately we don't have move-only
/// types yet. Therefore it's needed to call `deinitialize()` explicitly to
/// destruct this data structure, e.g. in a `defer {}` block.
struct BasicBlockSet : IntrusiveSet {

  private let context: BridgedPassContext
  private let bridged: BridgedBasicBlockSet
    
  init(_ context: some Context) {
    self.context = context._bridged
    self.bridged = self.context.allocBasicBlockSet()
  }

  func contains(_ block: BasicBlock) -> Bool {
    bridged.contains(block.bridged)
  }

  /// Returns true if `block` was not contained in the set before inserting.
  @discardableResult
  mutating func insert(_ block: BasicBlock) -> Bool {
    bridged.insert(block.bridged)
  }

  mutating func erase(_ block: BasicBlock) {
    bridged.erase(block.bridged)
  }

  var description: String {
    let function = bridged.getFunction().function
    let blockNames = function.blocks.enumerated().filter { contains($0.1) }
                                                 .map { "bb\($0.0)"}
    return "{" + blockNames.joined(separator: ", ") + "}"
  }

  /// TODO: once we have move-only types, make this a real deinit.
  mutating func deinitialize() {
    context.freeBasicBlockSet(bridged)
  }
}

/// A set of values.
///
/// This is an extremely efficient implementation which does not need memory
/// allocations or hash lookups.
///
/// This type should be a move-only type, but unfortunately we don't have move-only
/// types yet. Therefore it's needed to call `deinitialize()` explicitly to
/// destruct this data structure, e.g. in a `defer {}` block.
struct ValueSet : IntrusiveSet {

  private let context: BridgedPassContext
  private let bridged: BridgedNodeSet
    
  init(_ context: some Context) {
    self.context = context._bridged
    self.bridged = self.context.allocNodeSet()
  }

  func contains(_ value: Value) -> Bool {
    bridged.containsValue(value.bridged)
  }

  /// Returns true if `value` was not contained in the set before inserting.
  @discardableResult
  mutating func insert(_ value: Value) -> Bool {
    bridged.insertValue(value.bridged)
  }

  mutating func erase(_ value: Value) {
    bridged.eraseValue(value.bridged)
  }

  var description: String {
    let function = bridged.getFunction().function
    var d = "{\n"
    for block in function.blocks {
      for arg in block.arguments {
        if contains(arg) {
          d += arg.description
        }
      }
      for inst in block.instructions {
        for result in inst.results {
          if contains(result) {
            d += result.description
          }
        }
      }
    }
    d += "}\n"
    return d
  }

  /// TODO: once we have move-only types, make this a real deinit.
  mutating func deinitialize() {
    context.freeNodeSet(bridged)
  }
}

/// A set of instructions.
///
/// This is an extremely efficient implementation which does not need memory
/// allocations or hash lookups.
///
/// This type should be a move-only type, but unfortunately we don't have move-only
/// types yet. Therefore it's needed to call `deinitialize()` explicitly to
/// destruct this data structure, e.g. in a `defer {}` block.
struct InstructionSet : IntrusiveSet {

  private let context: BridgedPassContext
  private let bridged: BridgedNodeSet
    
  init(_ context: some Context) {
    self.context = context._bridged
    self.bridged = self.context.allocNodeSet()
  }

  func contains(_ inst: Instruction) -> Bool {
    bridged.containsInstruction(inst.bridged)
  }

  /// Returns true if `inst` was not contained in the set before inserting.
  @discardableResult
  mutating func insert(_ inst: Instruction) -> Bool {
    bridged.insertInstruction(inst.bridged)
  }

  mutating func erase(_ inst: Instruction) {
    bridged.eraseInstruction(inst.bridged)
  }

  var description: String {
    let function = bridged.getFunction().function
    var d = "{\n"
    for inst in function.instructions {
      if contains(inst) {
        d += inst.description
      }
    }
    d += "}\n"
    return d
  }

  /// TODO: once we have move-only types, make this a real deinit.
  mutating func deinitialize() {
    context.freeNodeSet(bridged)
  }
}

/// A set of operands.
///
/// This is an extremely efficient implementation which does not need memory
/// allocations or hash lookups.
///
/// This type should be a move-only type, but unfortunately we don't have move-only
/// types yet. Therefore it's needed to call `deinitialize()` explicitly to
/// destruct this data structure, e.g. in a `defer {}` block.
struct OperandSet : IntrusiveSet {

  private let context: BridgedPassContext
  private let bridged: BridgedOperandSet

  init(_ context: some Context) {
    self.context = context._bridged
    self.bridged = self.context.allocOperandSet()
  }

  func contains(_ operand: Operand) -> Bool {
    bridged.contains(operand.bridged)
  }

  /// Returns true if `inst` was not contained in the set before inserting.
  @discardableResult
  mutating func insert(_ operand: Operand) -> Bool {
    bridged.insert(operand.bridged)
  }

  mutating func erase(_ operand: Operand) {
    bridged.erase(operand.bridged)
  }

  var description: String {
    let function = bridged.getFunction().function
    var d = "{\n"
    for inst in function.instructions {
      for op in inst.operands {
        if contains(op) {
          d += op.description
        }
      }
    }
    d += "}\n"
    return d
  }

  /// TODO: once we have move-only types, make this a real deinit.
  mutating func deinitialize() {
    context.freeOperandSet(bridged)
  }
}

extension IntrusiveSet {
  mutating func insert(contentsOf source: some Sequence<Element>) {
    for element in source {
      _ = insert(element)
    }
  }

  init(insertContentsOf source: some Sequence<Element>, _ context: some Context) {
    self.init(context)
    insert(contentsOf: source)
  }
}
