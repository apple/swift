// RUN: %empty-directory(%t)
// RUN: %target-build-swift -strict-concurrency=complete  -Xfrontend -disable-availability-checking -parse-stdlib %s -module-name main -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: asserts

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import Swift
import StdlibUnittest
import _Concurrency

// TODO: Once transferring is enabled by default, move these into
// demangleToMetadata.swift.

class Klass {}

let DemangleToMetadataTests = TestSuite("DemangleToMetadata")

if #available(SwiftStdlib 6.0, *) {
  DemangleToMetadataTests.test("transferring parameter") {
    typealias Fn = (transferring Klass) -> Void
    expectEqual("y4main5KlassCnYuc", _mangledTypeName(Fn.self)!)
    expectEqual(Fn.self, _typeByName("y4main5KlassCnYuc")!)

    typealias Fn2 = (transferring Klass, transferring Klass) -> Void
    expectEqual("y4main5KlassCnYu_ACnYutc", _mangledTypeName(Fn2.self)!)
    expectEqual(Fn2.self, _typeByName("y4main5KlassCnYu_ACnYutc")!)

    typealias Fn3 = (transferring (Klass, Klass)) -> Void
    expectEqual("y4main5KlassC_ACtnYuc", _mangledTypeName(Fn3.self)!)
    expectEqual(Fn3.self, _typeByName("y4main5KlassC_ACtnYuc")!)
  }

  DemangleToMetadataTests.test("transferring result") {
    typealias Fn = (Klass) -> transferring Klass
    expectEqual("4main5KlassCACYTc", _mangledTypeName(Fn.self)!)
    expectEqual(Fn.self, _typeByName("4main5KlassCACYTc")!)

    typealias Fn2 = (transferring Klass, transferring Klass) -> transferring Klass
    expectEqual("4main5KlassCACnYu_ACnYutYTc", _mangledTypeName(Fn2.self)!)
    expectEqual(Fn2.self, _typeByName("4main5KlassCACnYu_ACnYutYTc")!)

    typealias Fn3 = (transferring (Klass, Klass)) -> transferring (Klass, Klass)
    expectEqual("4main5KlassC_ACtAC_ACtnYuYTc", _mangledTypeName(Fn3.self)!)
    expectEqual(Fn3.self, _typeByName("4main5KlassC_ACtAC_ACtnYuYTc")!)

    typealias Fn4 = () -> transferring Klass
    expectEqual("4main5KlassCyYTc", _mangledTypeName(Fn4.self)!)
    expectEqual(Fn4.self, _typeByName("4main5KlassCyYTc")!)
  }
}

runAllTests()
