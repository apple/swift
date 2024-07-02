// RUN: %target-run-simple-swift
// REQUIRES: executable_test
// REQUIRES: objc_interop

import StdlibUnittest
import Foundation
import CoreFoundation
import CoreLocation
import Dispatch

let DemangleToMetadataTests = TestSuite("DemangleToMetadataObjC")

@objc class C : NSObject { }
@objc(CRenamedInObjC) class CRenamed : NSObject { }
@objc enum E: Int { case a }
@objc protocol P1 { }
protocol P2 { }
@objc protocol P3: P1 { }
@objc protocol mainP4 { }

@objc(P5RenamedInObjC) protocol P5 { }

DemangleToMetadataTests.test("@objc classes") {
  expectEqual(type(of: C()), _typeByName("4main1CC")!)

  // @objc class that's been renamed, which can be found by its Objective-C
  // name...
  expectEqual(type(of: CRenamed()), _typeByName("So14CRenamedInObjCC")!)

  // ... but not by it's Swift name.
  expectNil(_typeByName("4main8CRenamed"))
}

DemangleToMetadataTests.test("@objc enums") {
  expectEqual(type(of: E.a), _typeByName("4main1EO")!)
}

func f1_composition_objc_protocol(_: P1) { }
func f1_composition_objc_protocol_P4(_: mainP4) { }

DemangleToMetadataTests.test("@objc protocols") {
  expectEqual(type(of: f1_composition_objc_protocol),
              _typeByName("y4main2P1_pc")!)
  expectEqual(type(of: f1_composition_objc_protocol_P4),
              _typeByName("y4main0A2P4_pc")!)
}

DemangleToMetadataTests.test("Objective-C classes") {
  expectEqual(type(of: NSObject()), _typeByName("So8NSObjectC")!)
}

func f1_composition_NSCoding(_: NSCoding) { }
func f1_composition_P5(_: P5) { }

DemangleToMetadataTests.test("Objective-C protocols") {
  expectEqual(type(of: f1_composition_NSCoding), _typeByName("ySo8NSCoding_pc")!)

  // @objc Swift protocols can be found by their Objective-C names...
  expectEqual(type(of: f1_composition_P5), _typeByName("ySo15P5RenamedInObjC_pc")!)

  // ... but not their Swift names.
  expectNil(_typeByName("y4main2P5_pc"))
}

DemangleToMetadataTests.test("Classes that don't exist") {
  expectNil(_typeByName("4main4BoomC"))
}

DemangleToMetadataTests.test("CoreFoundation classes") {
  expectEqual(CFArray.self, _typeByName("So10CFArrayRefa")!)
}

DemangleToMetadataTests.test("Imported error types") {
  expectEqual(URLError.self, _typeByName("10Foundation8URLErrorV")!)
  expectEqual(URLError.Code.self,
    _typeByName("10Foundation8URLErrorV4CodeV")!)
}

DemangleToMetadataTests.test("Imported swift_wrapper types") {
  expectEqual(URLFileResourceType.self,
    _typeByName("So21NSURLFileResourceTypea")!)
}

DemangleToMetadataTests.test("Imported enum types") {
  expectEqual(URLSessionTask.State.self,
    _typeByName("So21NSURLSessionTaskStateV")!)
}

class CG4<T: P1, U: P2> { }
extension C : P1 { }
extension C : P2 { }

class D: P2 { }

DemangleToMetadataTests.test("@objc protocol conformances") {
  expectEqual(CG4<C, C>.self,
    _typeByName("4main3CG4CyAA1CCAA1CCG")!)
  expectNil(_typeByName("4main3CG4CyAA1DCAA1DCG"))
}

DemangleToMetadataTests.test("synthesized declarations") {
  expectEqual(CLError.self, _typeByName("SC7CLErrorLeV")!)
  expectNil(_typeByName("SC7CLErrorV"))
  expectEqual(CLError.Code.self, _typeByName("So7CLErrorV")!)

  let error = NSError(domain: NSCocoaErrorDomain, code: 0)
  let reflectionString = String(reflecting: CLError(_nsError: error))
  expectTrue(reflectionString.hasPrefix("__C_Synthesized.related decl 'e' for CLError(_nsError:"))
}

DemangleToMetadataTests.test("members of runtime-only Objective-C classes") {
  expectNotNil(_typeByName("So17OS_dispatch_queueC8DispatchE10AttributesV"))
  expectEqual(DispatchQueue.Attributes.self,
    _typeByName("So17OS_dispatch_queueC8DispatchE10AttributesV")!)
}

DemangleToMetadataTests.test("runtime conformance lookup via foreign superclasses") {
  expectEqual(Set<CFMutableString>.self,
    _typeByName("ShySo18CFMutableStringRefaG")!)
}

class F<T: P1> { }

DemangleToMetadataTests.test("runtime conformance check for @objc protocol inheritance") {
  expectEqual(F<P3>.self, _typeByName("4main1FCyAA2P3PG")!)
}

DemangleToMetadataTests.test("Objective-C generics") {
  expectEqual(NSArray.self, _typeByName("So7NSArrayCySo8NSStringCG")!)
}

runAllTests()

