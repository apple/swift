// RUN: %target-run-simple-swift
// REQUIRES: executable_test
// REQUIRES: objc_interop

import StdlibUnittest
import Foundation
import CoreFoundation

let DemangleToMetadataTests = TestSuite("DemangleToMetadataObjC")

@objc class C : NSObject { }
@objc enum E: Int { case a }
@objc protocol P1 { }
protocol P2 { }

DemangleToMetadataTests.test("@objc classes") {
  expectEqual(type(of: C()), _typeByMangledName("4main1CC")!)
}

DemangleToMetadataTests.test("@objc enums") {
  expectEqual(type(of: E.a), _typeByMangledName("4main1EO")!)
}

func f1_composition_objc_protocol(_: P1) { }

DemangleToMetadataTests.test("@objc protocols") {
  expectEqual(type(of: f1_composition_objc_protocol),
              _typeByMangledName("yy4main2P1_pc")!)
}

DemangleToMetadataTests.test("Objective-C classes") {
  expectEqual(type(of: NSObject()), _typeByMangledName("So8NSObjectC")!)
}

func f1_composition_NSCoding(_: NSCoding) { }

DemangleToMetadataTests.test("Objective-C protocols") {
  expectEqual(type(of: f1_composition_NSCoding), _typeByMangledName("yySo8NSCoding_pc")!)
}

DemangleToMetadataTests.test("Classes that don't exist") {
  expectNil(_typeByMangledName("4main4BoomC"))
}

DemangleToMetadataTests.test("CoreFoundation classes") {
  expectEqual(CFArray.self, _typeByMangledName("So10CFArrayRefa")!)
}

DemangleToMetadataTests.test("Imported error types") {
  expectEqual(URLError.self, _typeByMangledName("10Foundation8URLErrorV")!)
  expectEqual(URLError.Code.self,
    _typeByMangledName("10Foundation8URLErrorV4CodeV")!)
}

DemangleToMetadataTests.test("Imported swift_wrapper types") {
  expectEqual(URLFileResourceType.self,
    _typeByMangledName("So21NSURLFileResourceTypea")!)
}

DemangleToMetadataTests.test("Imported enum types") {
  expectEqual(NSURLSessionTask.State.self,
    _typeByMangledName("So21NSURLSessionTaskStateV")!)
}

runAllTests()

