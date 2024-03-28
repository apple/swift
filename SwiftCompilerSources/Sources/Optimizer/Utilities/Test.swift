//===----------- Test.swift - In-IR tests from Swift source ---------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// TO ADD A NEW TEST, just add a new FunctionTest instance.
// - In the source file containing the functionality you want to test:
//       let myNewTest = 
//       FunctionTest("my_new_test") { function, arguments, context in
//       }
// - In SwiftCompilerSources/Sources/SIL/Test.swift's registerOptimizerTests
//   function, add a new argument to the variadic function:
//       registerFunctionTests(..., myNewTest)
//
//===----------------------------------------------------------------------===//
//
// TO TROUBLESHOOT A NEW TEST, consider the following scenarios:
// - PROBLEM: The test isn't running on PLATFORM and the failure says
//
//                Found no test named my_new_test.
//
//   SOLUTION: Is this a platform one that doesn't use SwiftCompilerSources
//             (e.g. Windows)?  Then add
//
//                 // REQUIRES: swift_in_compiler
//
//             to the test file.
//   EXPLANATION: The tests written within SwiftCompilerSources only get built
//                and registered on platforms where the SwiftCompilerSources are
//                built and used.
//
//===----------------------------------------------------------------------===//
//
// Provides a mechanism for writing tests against compiler code in the context
// of a function.  The goal is to get the same effect as calling a function and
// checking its output.
//
// This is done via the specify_test instruction.  Using one or more instances
// of it in your test case's SIL function, you can specify which test (instance
// of FunctionTest) should be run and what arguments should be provided to it.
// For full details of the specify_test instruction's grammar, see SIL.rst.
//
// The test grabs the arguments it expects out of the TestArguments instance
// it is provided.  It calls some function or functions.  It then prints out
// interesting results.  These results can then be FileCheck'd.
//
// CASE STUDY:
// Here's an example of how it works:
// 0) A source file, NeatUtils.swift contains
//
//    fileprivate func myNeatoUtility(int: Int, value: Value, function: Function) { ... }
//
//    and
//
//    let myNeatoUtilityTest = 
//    FunctionTest("my_neato_utility") { function, arguments, test in
//         // The code here is described in detail below.
//         // See 4).
//         let count = arguments.takeInt()
//         let target = arguments.takeValue()
//         let callee = arguments.takeFunction()
//         // See 5)
//         myNeatoUtility(int: count, value: target, function: callee)
//         // See 6)
//         print(function)
//      }
// 1) A test test/SILOptimizer/interesting_functionality_unit.sil runs the
//    TestRunner pass:
//     // RUN: %target-sil-opt -test-runner %s -o /dev/null 2>&1 | %FileCheck %s
//     // REQUIRES: swift_in_compiler
// 2) A function in interesting_functionality_unit.sil contains the
//    specify_test instruction.
//      sil @f : $() -> () {
//      ...
//      specify_test "my_neato_utility 43 %2 @function[other_fun]"
//      ...
//      }
// 3) TestRunner finds the FunctionTest instance myNeatoUtilityTest registered
//    under the name "my_neato_utility", and calls run() on it, passing the
//    passing first the function, last the FunctionTest instance, AND in the
//    middle, most importantly a TestArguments instance that contains
//
//      (43 : Int, someValue : Value, other_fun : Function)
//
// 4) myNeatoUtilityTest calls takeUInt(), takeValue(), and takeFunction() on
//    the test::Arguments instance.
//      let count = arguments.takeInt()
//      let target = arguments.takeValue()
//      let callee = arguments.takeFunction()
// 5) myNeatoUtilityTest calls myNeatoUtility, passing these values along.
//      myNeatoUtility(int: count, value: target, function: callee)
// 6) myNeatoUtilityTest then dumps out the current function, which was modified
//    in the process.
//      print(function)
// 7) The test file test/SILOptimizer/interesting_functionality_unit.sil matches
// the
//    expected contents of the modified function:
//    // CHECK-LABEL: sil @f
//    // CHECK-NOT:     function_ref @other_fun
//
//===----------------------------------------------------------------------===//

import Basic
import SIL
import SILBridging
import OptimizerBridging

/// The primary interface to in-IR tests.
struct FunctionTest {
  let name: String
  let invocation: FunctionTestInvocation

  public init(_ name: String, invocation: @escaping FunctionTestInvocation) {
    self.name = name
    self.invocation = invocation
  }
}

/// The type of the closure passed to a FunctionTest.
typealias FunctionTestInvocation = @convention(thin) (Function, TestArguments, FunctionPassContext) -> ()

/// Wraps the arguments specified in the specify_test instruction.
public struct TestArguments {
  public var bridged: BridgedTestArguments
  fileprivate init(bridged: BridgedTestArguments) {
    self.bridged = bridged
  }

  public var hasUntaken: Bool { bridged.hasUntaken() }
  public func takeString() -> StringRef { StringRef(bridged: bridged.takeString()) }
  public func takeBool() -> Bool { bridged.takeBool() }
  public func takeInt() -> Int { bridged.takeInt() }
  public func takeOperand() -> Operand { Operand(bridged: bridged.takeOperand()) }
  public func takeValue() -> Value { bridged.takeValue().value }
  public func takeInstruction() -> Instruction { bridged.takeInstruction().instruction }
  public func takeArgument() -> Argument { bridged.takeArgument().argument }
  public func takeBlock() -> BasicBlock { bridged.takeBlock().block }
  public func takeFunction() -> Function { bridged.takeFunction().function }
}

extension BridgedTestArguments {
  public var native: TestArguments { TestArguments(bridged: self) }
}

/// Registration of each test in the SIL module.
public func registerOptimizerTests() {
  // Register each test.
  registerFunctionTests(
    argumentConventionsTest,
    borrowIntroducersTest,
    enclosingValuesTest,
    forwardingDefUseTest,
    forwardingUseDefTest,
    interiorLivenessTest,
    lifetimeDependenceRootTest,
    lifetimeDependenceScopeTest,
    lifetimeDependenceUseTest,
    linearLivenessTest,
    parseTestSpecificationTest,
    variableIntroducerTest,
    gatherCallSitesTest
  )

  // Finally register the thunk they all call through.
  registerFunctionTestThunk(functionTestThunk)
}

private func registerFunctionTests(_ tests: FunctionTest...) {
  tests.forEach { registerFunctionTest($0) }
}

private func registerFunctionTest(_ test: FunctionTest) {
  test.name._withBridgedStringRef { ref in
    registerFunctionTest(ref, castToOpaquePointer(fromInvocation: test.invocation))
  }
}

/// The function called by the swift::test::FunctionTest which invokes the
/// actual test function.
///
/// This function is necessary because tests need to be written in terms of
/// native Swift types (Function, TestArguments, BridgedPassContext)
/// rather than their bridged variants, but such a function isn't representable
/// in C++.  This thunk unwraps the bridged types and invokes the real
/// function.
private func functionTestThunk(
  _ erasedInvocation: UnsafeMutableRawPointer,
  _ function: BridgedFunction, 
  _ arguments: BridgedTestArguments, 
  _ passInvocation: BridgedSwiftPassInvocation) {
  let invocation = castToInvocation(fromOpaquePointer: erasedInvocation)
  let context = FunctionPassContext(_bridged: BridgedPassContext(invocation: passInvocation.invocation))
  invocation(function.function, arguments.native, context)
  fflush(stdout)
}

/// Bitcast a thin test closure to void *.
///
/// Needed so that the closure can be represented in C++ for storage in the test
/// registry.
private func castToOpaquePointer(fromInvocation invocation: FunctionTestInvocation) -> UnsafeMutableRawPointer {
  return unsafeBitCast(invocation, to: UnsafeMutableRawPointer.self)
}

/// Bitcast a void * to a thin test closure.
///
/// Needed so that the closure stored in the C++ test registry can be invoked
/// via the functionTestThunk.
private func castToInvocation(fromOpaquePointer erasedInvocation: UnsafeMutableRawPointer) -> FunctionTestInvocation {
  return unsafeBitCast(erasedInvocation, to: FunctionTestInvocation.self)
}

// Arguments:
// - string: list of characters, each of which specifies subsequent arguments
//           - A: (block) argument
//           - F: function
//           - B: block
//           - I: instruction
//           - V: value
//           - O: operand
//           - b: boolean
//           - u: unsigned
//           - s: string
// - ...
// - an argument of the type specified in the initial string
// - ...
// Dumps:
// - for each argument (after the initial string)
//   - its type
//   - something to identify the instance (mostly this means calling dump)
let parseTestSpecificationTest = 
FunctionTest("test_specification_parsing") { function, arguments, context in 
  let expectedFields = arguments.takeString()
  for expectedField in expectedFields.string {
    switch expectedField {
    case "A":
      let argument = arguments.takeArgument()
      print("argument:\n\(argument)")
    case "F":
      let function = arguments.takeFunction()
      print("function: \(function.name)")
    case "B":
      let block = arguments.takeBlock()
      print("block:\n\(block)")
    case "I":
      let instruction = arguments.takeInstruction()
      print("instruction: \(instruction)")
    case "V":
      let value = arguments.takeValue()
      print("value: \(value)")
    case "O":
      let operand = arguments.takeOperand()
      print("operand: \(operand)")
    case "u":
      let u = arguments.takeInt()
      print("uint: \(u)")
    case "b":
      let b = arguments.takeBool()
      print("bool: \(b)")
    case "s":
      let s = arguments.takeString()
      print("string: \(s)")
    default:
      fatalError("unknown field type was expected?!");
    }
  }
}
