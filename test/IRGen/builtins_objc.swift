// RUN: %target-swift-frontend %use_no_opaque_pointers -parse-stdlib -primary-file %s -emit-ir > %t.ir
// RUN: %target-swift-frontend -parse-stdlib -primary-file %s -emit-ir
// RUN: %FileCheck %s --input-file=%t.ir

// REQUIRES: executable_test
// REQUIRES: objc_interop

import Swift
import Foundation
import CoreGraphics

@objc enum ObjCEnum: Int32 { case X }
@objc class ObjCClass: NSObject {}
class NonObjCClass {}

@_silgen_name("use")
func use(_: Builtin.RawPointer)

func getObjCTypeEncoding<T>(_: T) {
  // CHECK: call swiftcc void @use({{.* i8]\*}} @.str.1.i,
  use(Builtin.getObjCTypeEncoding(Int32.self))
  // CHECK: call swiftcc void @use({{.* i8]\*}} @.str.1.i
  use(Builtin.getObjCTypeEncoding(ObjCEnum.self))
  // CHECK: call swiftcc void @use({{.* i8]\*}} [[CGRECT:@".str.[0-9]+.\{CGRect=[^"]*"]],
  use(Builtin.getObjCTypeEncoding(CGRect.self))
  // CHECK: call swiftcc void @use({{.* i8]\*}} [[NSRANGE:@".str.[0-9]+.\{_NSRange=[^"]*"]],
  use(Builtin.getObjCTypeEncoding(NSRange.self))
  // CHECK: call swiftcc void @use({{.* i8]\*}} @".str.1.@"
  use(Builtin.getObjCTypeEncoding(AnyObject.self))
  // CHECK: call swiftcc void @use({{.* i8]\*}} @".str.1.@"
  use(Builtin.getObjCTypeEncoding(NSObject.self))
  // CHECK: call swiftcc void @use({{.* i8]\*}} @".str.1.@"
  use(Builtin.getObjCTypeEncoding(ObjCClass.self))
  // CHECK: call swiftcc void @use({{.* i8]\*}} @".str.1.@"
  use(Builtin.getObjCTypeEncoding(NonObjCClass.self))
}

