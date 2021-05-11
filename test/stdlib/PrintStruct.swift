// RUN: %empty-directory(%t)
// RUN: %target-build-swift -c -whole-module-optimization -parse-as-library -emit-module -emit-module-path %t/PrintTestTypes.swiftmodule -o %t/PrintTestTypes.o %S/Inputs/PrintTestTypes.swift
// RUN: %target-build-swift %s -Xlinker %t/PrintTestTypes.o -I %t -L %t -o %t/main
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main
// REQUIRES: executable_test
//
// FIXME Disable this case because this failed on only CI.
// Failed with 'Caught exception of type "CallIndirectOOB"'
// UNSUPPORTED: CPU=wasm32

import StdlibUnittest
import PrintTestTypes


let PrintTests = TestSuite("PrintStruct")

PrintTests.test("Printable") {
  let s0 = [ WithoutDescription(1), WithoutDescription(2), WithoutDescription(3) ]
  expectPrinted(
    "[PrintTestTypes.WithoutDescription(x: 1), PrintTestTypes.WithoutDescription(x: 2), PrintTestTypes.WithoutDescription(x: 3)]",
  s0)
  expectDebugPrinted(
  "[PrintTestTypes.WithoutDescription(x: 1), PrintTestTypes.WithoutDescription(x: 2), PrintTestTypes.WithoutDescription(x: 3)]",
  s0)
  
  expectPrinted("EmptyStructWithoutDescription()",
    EmptyStructWithoutDescription())
  expectDebugPrinted("PrintTestTypes.EmptyStructWithoutDescription()",
    EmptyStructWithoutDescription())
  
  expectPrinted(
    "ValuesWithoutDescription<Double, String, Array<Int>>(t: 1.25, u: \"abc\", v: [1, 2, 3])",
    ValuesWithoutDescription(1.25, "abc", [ 1, 2, 3 ]))
  expectDebugPrinted(
    "PrintTestTypes.ValuesWithoutDescription<Swift.Double, Swift.String, Swift.Array<Swift.Int>>(t: 1.25, u: \"abc\", v: [1, 2, 3])", ValuesWithoutDescription(1.25, "abc", [ 1, 2, 3 ]))
}

PrintTests.test("custom string convertible structs") {
  struct Wrapper : CustomStringConvertible {
    var x: CustomStringConvertible?
    
    var description: String {
      return "Wrapper(\(x.debugDescription))"
    }
  }

  expectPrinted("Wrapper(nil)", Wrapper())
  expectPrinted("Wrapper(Optional(Wrapper(nil)))",
    Wrapper(x: Wrapper()))
  expectPrinted("Wrapper(Optional(Wrapper(Optional(Wrapper(nil)))))",
    Wrapper(x: Wrapper(x: Wrapper())))
}

func test_ThickMetatypePrintingImpl<T>(
  _ thickMetatype: T.Type,
  _ expectedPrint: String,
  _ expectedDebug: String
  ) {
    expectPrinted(expectedPrint, thickMetatype)
    expectPrinted("[\(expectedDebug)]", [thickMetatype])
    expectDebugPrinted(expectedDebug, thickMetatype)
    expectDebugPrinted("[\(expectedDebug)]", [thickMetatype])
}

PrintTests.test("StructPrintable") {
  let s0 = StructPrintable(1)
  let s1: ProtocolUnrelatedToPrinting = StructPrintable(1)
  let s2: CustomStringConvertible = StructPrintable(1)
  let s3: Any = StructPrintable(1)
  
  expectPrinted("►1◀︎", s0)
  expectPrinted("►1◀︎", s1)
  expectPrinted("►1◀︎", s2)
  expectPrinted("►1◀︎", s3)
  
  let structMetatype = StructPrintable.self
  expectPrinted("StructPrintable", structMetatype)
  expectDebugPrinted("PrintTestTypes.StructPrintable", structMetatype)
  expectPrinted("[PrintTestTypes.StructPrintable]", [structMetatype])
  expectDebugPrinted("[PrintTestTypes.StructPrintable]", [structMetatype])
  test_ThickMetatypePrintingImpl(structMetatype, "StructPrintable",
    "PrintTestTypes.StructPrintable")
}

PrintTests.test("LargeStructPrintable") {
  let s0 = LargeStructPrintable(10, 20, 30, 40)
  let s1: ProtocolUnrelatedToPrinting = LargeStructPrintable(10, 20, 30, 40)
  let s2: CustomStringConvertible = LargeStructPrintable(10, 20, 30, 40)
  let s3: Any = LargeStructPrintable(10, 20, 30, 40)

  expectPrinted("<10 20 30 40>", s0)
  expectPrinted("<10 20 30 40>", s1)
  expectPrinted("<10 20 30 40>", s2)
  expectPrinted("<10 20 30 40>", s0)
  expectPrinted("<10 20 30 40>", s3)
}

PrintTests.test("StructVeryPrintable") {
  let s0 = StructVeryPrintable(1)
  let s1: ProtocolUnrelatedToPrinting = StructVeryPrintable(1)
  let s2: CustomStringConvertible = StructVeryPrintable(1)
  let s3: CustomDebugStringConvertible = StructVeryPrintable(1)
  let s4: Any = StructVeryPrintable(1)
  
  expectPrinted("<description: 1>", s0)
  expectPrinted("<description: 1>", s1)
  expectPrinted("<description: 1>", s2)
  expectPrinted("<description: 1>", s3)
  expectPrinted("<description: 1>", s4)
}

runAllTests()
