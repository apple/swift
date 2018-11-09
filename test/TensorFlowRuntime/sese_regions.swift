// This test has various test cases to check that SESE regions are computed correctly.
// RUN: %target-swift-frontend -Xllvm -tf-dump-intermediates -emit-sil -O %s -verify | %FileCheck %s
// RUN: %target-run-simple-swift
// REQUIRES: executable_test
// REQUIRES: swift_test_mode_optimize

import TensorFlow
import TensorFlowUnittest
import StdlibUnittest

var SESERegionTests = TestSuite("SESERegion")

enum TestError : Error {
  case NegativeOutOfBound
  case PositiveOutOfBound
}

public func testFunctionRegion(_ count : Int32) -> Tensor<Int32> {
  var result = Tensor<Int32>(0)
  do {
    if count > 0 {
      if count < 100 {
        // expected-warning @+2 {{value implicitly copied to the host}}
        // expected-warning @+1 {{value implicitly copied to the host}}
        result += 1
      } else {
        result += 2
        throw TestError.PositiveOutOfBound
      }
    } else {
      if count > -100 {
        // expected-warning @+2 {{value implicitly copied to the host}}
        // expected-warning @+1 {{value implicitly copied to the host}}
        result += 3
      } else {
        result += 4
        throw TestError.NegativeOutOfBound
      }
    } // expected-note {{value used here}}
    // expected-note @+1 {{value used here}}
  } catch { // expected-note {{value used here}}
    // expected-warning @+2 {{value implicitly copied to the host}}
    // expected-warning @+1 {{value implicitly copied to the host}}
    result += 5
  }
  return result
}

// expected-note @+3 {{value used here}}
// expected-note @+2 {{value used here}}
// expected-note @+1 {{value used here}}
SESERegionTests.testAllBackends("testFunctionRegion") { 
  expectEqualWithScalarTensor(1, testFunctionRegion(99))
  expectEqualWithScalarTensor(7, testFunctionRegion(101))
  expectEqualWithScalarTensor(3, testFunctionRegion(-99))
  expectEqualWithScalarTensor(9, testFunctionRegion(-101))
}

// CHECK-LABEL:--- XLA CFG Canonicalize: {{.*}}testFunctionRegion{{.*}}
// CHECK:[sequence
// CHECK:  {condition Header: {{bb[0-9]+}}
// CHECK:    {condition Header: {{bb[0-9]+}}
// CHECK:      block {{bb[0-9]+}}
// CHECK:      [sequence
// CHECK:        block {{bb[0-9]+}}
// CHECK:        {shared
// CHECK:          block [[FBLK:bb[0-9]+]]
// CHECK:        }]}
// CHECK:    {condition Header: {{bb[0-9]+}}
// CHECK:      block {{bb[0-9]+}}
// CHECK:      [sequence
// CHECK:        block {{bb[0-9]+}}
// CHECK:        {shared
// CHECK:          block [[FBLK]]
// CHECK:        }]}}
// CHECK:  block {{bb[0-9]+}}]
// CHECK:--- XLA CFG Canonicalize end

public func testFunctionRegionWithLoop(_ count : Int32) -> Tensor<Int32> {
  var result = Tensor<Int32>(0)
  do {
    if count > 0 {
      if count < 100 {
        // expected-warning @+2 {{value implicitly copied to the host}}
        // expected-warning @+1 {{value implicitly copied to the host}}
        result += 1
      } else {
        result += 2
        throw TestError.PositiveOutOfBound
      }
    } else {
      if count > -100 {
        // expected-warning @+2 {{value implicitly copied to the host}}
        // expected-warning @+1 {{value implicitly copied to the host}}
        result += 3
      } else {
        result += 4
        throw TestError.NegativeOutOfBound
      }
    } // expected-note {{value used here}}
    // expected-note @+1 {{value used here}}
  } catch { // expected-note {{value used here}}
    var i: Int32 = 0
    while i < 2 {
      // expected-warning @+2 {{value implicitly copied to the host}}
      // expected-warning @+1 {{value implicitly copied to the host}}
      result += 5
      i += 1
    }
  }
  return result
}

// expected-note @+3 {{value used here}}
// expected-note @+2 {{value used here}}
// expected-note @+1 {{value used here}}
SESERegionTests.testAllBackends("testFunctionRegionWithLoop") { 
  expectEqualWithScalarTensor(1, testFunctionRegionWithLoop(99))
  expectEqualWithScalarTensor(12, testFunctionRegionWithLoop(101))
  expectEqualWithScalarTensor(3, testFunctionRegionWithLoop(-99))
  expectEqualWithScalarTensor(14, testFunctionRegionWithLoop(-101))
}


// CHECK-LABEL:--- XLA CFG Canonicalize: {{.*}}testFunctionRegionWithLoop{{.*}}
// CHECK:[sequence
// CHECK:  {condition Header: {{bb[0-9]+}}
// CHECK:    {condition Header: {{bb[0-9]+}}
// CHECK:      block {{bb[0-9]+}}
// CHECK:      [sequence
// CHECK:        block {{bb[0-9]+}}
// CHECK:        {shared
// CHECK:          [sequence
// CHECK:            <while Preheader: [[PHDR:bb[0-9]+]], Header: [[HDR:bb[0-9]+]], exit: [[EXIT:bb[0-9]+]]
// CHECK:              [sequence
// CHECK:                {condition Header: [[COND:bb[0-9]+]]
// CHECK:                  block [[TRUEBLK:bb[0-9]+]]
// CHECK:                  block [[FALSEBLK:bb[0-9]+]]}
// CHECK:                block [[MERGEBLK:bb[0-9]+]]]>
// CHECK:            block [[LATCH:bb[0-9]+]]
// CHECK:        }]}
// CHECK:    {condition Header: {{bb[0-9]+}}
// CHECK:      block {{bb[0-9]+}}
// CHECK:      [sequence
// CHECK:        block {{bb[0-9]+}}
// CHECK:        {shared
// CHECK:          [sequence
// CHECK:            <while Preheader: [[PHDR]], Header: [[HDR]], exit: [[EXIT]]
// CHECK:              [sequence
// CHECK:                {condition Header: [[COND]]
// CHECK:                  block [[TRUEBLK]]
// CHECK:                  block [[FALSEBLK]]}
// CHECK:                block [[MERGEBLK]]]>
// CHECK:            block [[LATCH]]]
// CHECK:        }]}}
// CHECK:  block {{bb[0-9]+}}]
// CHECK:--- XLA CFG Canonicalize end

runAllTests()
