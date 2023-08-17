//===--- EscapeInfoDumper.swift - Dumps escape information ----------------===//
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

/// Dumps the results of escape analysis.
///
/// Dumps the EscapeInfo query results for all `alloc_stack` instructions in a function.
///
/// This pass is used for testing EscapeInfo.
let escapeInfoDumper = FunctionPass(name: "dump-escape-info") {
  (function: Function, context: FunctionPassContext) in

  print("Escape information for \(function.name):")
  
  struct Visitor : EscapeVisitorWithResult {
    var result: Set<String> =  Set()
    
    mutating func visitUse(operand: Operand, path: EscapePath) -> UseResult {
      if operand.instruction is ReturnInst {
        result.insert("return[\(path.projectionPath)]")
        return .ignore
      }
      return .continueWalk
    }
    
    mutating func visitDef(def: Value, path: EscapePath) -> DefResult {
      guard let arg = def as? FunctionArgument else {
        return .continueWalkUp
      }
      result.insert("arg\(arg.index)[\(path.projectionPath)]")
      return .walkDown
    }
  }
  
  
  for inst in function.instructions {
    if let allocRef = inst as? AllocRefInst {
      
      let resultStr: String
      if let result = allocRef.visit(using: Visitor(), context) {
        if result.isEmpty {
          resultStr = " -    "
        } else {
          resultStr = Array(result).sorted().joined(separator: ",")
        }
      } else {
        resultStr = "global"
      }
      print("\(resultStr): \(allocRef)")
    }
  }
  print("End function \(function.name)\n")
}

/// Dumps the results of address-related escape analysis.
///
/// Dumps the EscapeInfo query results for addresses escaping to function calls.
/// The `fix_lifetime` instruction is used as marker for addresses and values to query.
///
/// This pass is used for testing EscapeInfo.
let addressEscapeInfoDumper = FunctionPass(name: "dump-addr-escape-info") {
  (function: Function, context: FunctionPassContext) in

  print("Address escape information for \(function.name):")

  var valuesToCheck = [Value]()
  var applies = [Instruction]()
  
  for inst in function.instructions {
    switch inst {
      case let fli as FixLifetimeInst:
        valuesToCheck.append(fli.operand.value)
      case is FullApplySite:
        applies.append(inst)
      default:
        break
    }
  }
  
  struct Visitor : EscapeVisitor {
    let apply: Instruction
    mutating func visitUse(operand: Operand, path: EscapePath) -> UseResult {
      let user = operand.instruction
      if user == apply {
        return .abort
      }
      if user is ReturnInst {
        // Anything which is returned cannot escape to an instruction inside the function.
        return .ignore
      }
      return .continueWalk
    }

    var followTrivialTypes: Bool { true }
    var followLoads: Bool { false }
  }

  // test `isEscaping(addressesOf:)`
  for value in valuesToCheck {
    print("value:\(value)")
    for apply in applies {
      let path = AliasAnalysis.getPtrOrAddressPath(for: value)
      
      if value.at(path).isEscaping(using: Visitor(apply: apply), context) {
        print("  ==> \(apply)")
      } else {
        print("  -   \(apply)")
      }
    }
  }
  
  // test `canReferenceSameField` for each pair of `fix_lifetime`.
  if !valuesToCheck.isEmpty {
    for lhsIdx in 0..<(valuesToCheck.count - 1) {
      for rhsIdx in (lhsIdx + 1) ..< valuesToCheck.count {
        print("pair \(lhsIdx) - \(rhsIdx)")
        let lhs = valuesToCheck[lhsIdx]
        let rhs = valuesToCheck[rhsIdx]
        print(lhs)
        print(rhs)

        let projLhs = lhs.at(AliasAnalysis.getPtrOrAddressPath(for: lhs))
        let projRhs = rhs.at(AliasAnalysis.getPtrOrAddressPath(for: rhs))
        let mayAlias = projLhs.canAddressAlias(with: projRhs, context)
        if mayAlias != projRhs.canAddressAlias(with: projLhs, context) {
          fatalError("canAddressAlias(with:) must be symmetric")
        }

        let addrReachable: Bool
        if lhs.type.isAddress && !rhs.type.isAddress {
          let anythingReachableFromRhs = rhs.at(SmallProjectionPath(.anything))
          addrReachable = projLhs.canAddressAlias(with: anythingReachableFromRhs, context)
          if mayAlias && !addrReachable {
            fatalError("mayAlias implies addrReachable")
          }
        } else {
          addrReachable = false
        }
        if mayAlias {
          print("may alias")
        } else if addrReachable {
          print("address reachable but no alias")
        } else {
          print("no alias")
        }
      }
    }
  }
  
  print("End function \(function.name)\n")
}
