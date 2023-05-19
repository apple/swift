// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -emit-module -o %t %S/Inputs/objc_protocols_Bas.swift
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -primary-file %s -emit-ir -disable-objc-attr-requires-foundation-module -target %target-swift-abi-5.8-triple > %t/out.ir
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -primary-file %s -emit-ir -disable-objc-attr-requires-foundation-module -target %target-swift-abi-5.7-triple > %t/out.old.ir
// RUN: %FileCheck --input-file=%t/out.ir %s --check-prefix=CHECK --check-prefix=CHECK-%target-os
// RUN: %FileCheck --input-file=%t/out.old.ir %s --check-prefix=CHECK-OLD --check-prefix=CHECK-%target-os-OLD

// REQUIRES: PTRSIZE=64
// REQUIRES: objc_interop

import gizmo
import objc_protocols_Bas

// -- Protocol "Frungible" inherits only objc protocols and should have no
//    out-of-line inherited witnesses in its witness table.
// CHECK: [[ZIM_FRUNGIBLE_WITNESS:@"\$s14objc_protocols3ZimCAA9FrungibleAAWP"]] = hidden constant [2 x i8*] [
// CHECK:    i8* bitcast ({{.*}}* @"$s14objc_protocols3ZimCAA9FrungibleA2aDP6frungeyyFTW{{(\.ptrauth)?}}" to i8*)
// CHECK: ]

protocol Ansible {
  func anse()
}

class Foo : NSRuncing, NSFunging, Ansible {
  @objc func runce() {}
  @objc func funge() {}
  @objc func foo() {}
  func anse() {}
}
// CHECK: @_INSTANCE_METHODS__TtC14objc_protocols3Foo = internal constant { i32, i32, [3 x { i8*, i8*, i8* }] } {
// CHECK:   i32 24, i32 3,
// CHECK:   [3 x { i8*, i8*, i8* }] [
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([6 x i8], [6 x i8]* @"\01L_selector_data(runce)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$s14objc_protocols3FooC5runceyyFTo{{(\.ptrauth)?}}" to i8*) },
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([6 x i8], [6 x i8]* @"\01L_selector_data(funge)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$s14objc_protocols3FooC5fungeyyFTo{{(\.ptrauth)?}}" to i8*) },
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([4 x i8], [4 x i8]* @"\01L_selector_data(foo)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$s14objc_protocols3FooC3fooyyFTo{{(\.ptrauth)?}}" to i8*)
// CHECK:   }]
// CHECK: }, section "__DATA, {{.*}}", align 8

class Bar {
  func bar() {}
}

// -- Bar does not directly have objc methods...
// CHECK-NOT: @_INSTANCE_METHODS_Bar

extension Bar : NSRuncing, NSFunging {
  @objc func runce() {}
  @objc func funge() {}
  @objc func foo() {}

  func notObjC() {}
}

// -- ...but the ObjC protocol conformances on its extension add some
// CHECK: @"_CATEGORY_INSTANCE_METHODS__TtC14objc_protocols3Bar_$_objc_protocols" = internal constant { i32, i32, [3 x { i8*, i8*, i8* }] } {
// CHECK:   i32 24, i32 3,
// CHECK:   [3 x { i8*, i8*, i8* }] [
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([6 x i8], [6 x i8]* @"\01L_selector_data(runce)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$s14objc_protocols3BarC5runceyyFTo{{(\.ptrauth)?}}" to i8*) },
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([6 x i8], [6 x i8]* @"\01L_selector_data(funge)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$s14objc_protocols3BarC5fungeyyFTo{{(\.ptrauth)?}}" to i8*) },
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([4 x i8], [4 x i8]* @"\01L_selector_data(foo)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$s14objc_protocols3BarC3fooyyFTo{{(\.ptrauth)?}}" to i8*) }
// CHECK:   ]
// CHECK: }, section "__DATA, {{.*}}", align 8

// class Bas from objc_protocols_Bas module
extension Bas : NSRuncing {
  // -- The runce() implementation comes from the original definition.
  @objc public
  func foo() {}
}

// CHECK: @"_CATEGORY_INSTANCE_METHODS__TtC18objc_protocols_Bas3Bas_$_objc_protocols" = internal constant { i32, i32, [1 x { i8*, i8*, i8* }] } {
// CHECK:   i32 24, i32 1,
// CHECK:   [1 x { i8*, i8*, i8* }] [
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([4 x i8], [4 x i8]* @"\01L_selector_data(foo)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$s18objc_protocols_Bas0C0C0a1_B0E3fooyyFTo{{(\.ptrauth)?}}" to i8*) }
// CHECK:   ]
// CHECK: }, section "__DATA, {{.*}}", align 8

// -- Swift protocol refinement of ObjC protocols.
protocol Frungible : NSRuncing, NSFunging {
  func frunge()
}

class Zim : Frungible {
  @objc func runce() {}
  @objc func funge() {}
  @objc func foo() {}

  func frunge() {}
}

// CHECK: @_INSTANCE_METHODS__TtC14objc_protocols3Zim = internal constant { i32, i32, [3 x { i8*, i8*, i8* }] } {
// CHECK:   i32 24, i32 3,
// CHECK:   [3 x { i8*, i8*, i8* }] [
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([6 x i8], [6 x i8]* @"\01L_selector_data(runce)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$s14objc_protocols3ZimC5runceyyFTo{{(\.ptrauth)?}}" to i8*) },
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([6 x i8], [6 x i8]* @"\01L_selector_data(funge)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$s14objc_protocols3ZimC5fungeyyFTo{{(\.ptrauth)?}}" to i8*) },
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([4 x i8], [4 x i8]* @"\01L_selector_data(foo)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$s14objc_protocols3ZimC3fooyyFTo{{(\.ptrauth)?}}" to i8*) }
// CHECK:   ]
// CHECK: }, section "__DATA, {{.*}}", align 8

// class Zang from objc_protocols_Bas module
extension Zang : Frungible {
  @objc public
  func runce() {}
  // funge() implementation from original definition of Zang
  @objc public
  func foo() {}

  func frunge() {}
}

// CHECK: @"_CATEGORY_INSTANCE_METHODS__TtC18objc_protocols_Bas4Zang_$_objc_protocols" = internal constant { i32, i32, [2 x { i8*, i8*, i8* }] } {
// CHECK:   i32 24, i32 2,
// CHECK:   [2 x { i8*, i8*, i8* }] [
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([6 x i8], [6 x i8]* @"\01L_selector_data(runce)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$s18objc_protocols_Bas4ZangC0a1_B0E5runceyyFTo{{(\.ptrauth)?}}" to i8*) },
// CHECK:     { i8*, i8*, i8* } { i8* getelementptr inbounds ([4 x i8], [4 x i8]* @"\01L_selector_data(foo)", i64 0, i64 0), i8* getelementptr inbounds ([8 x i8], [8 x i8]* @".str.7.v16@0:8", i64 0, i64 0), i8* bitcast ({{.*}}* @"$s18objc_protocols_Bas4ZangC0a1_B0E3fooyyFTo{{(\.ptrauth)?}}" to i8*) }
// CHECK:   ]
// CHECK: }, section "__DATA, {{.*}}", align 8

@objc protocol BaseProtocol { }
protocol InheritingProtocol : BaseProtocol { }
// -- Make sure that base protocol conformance is registered
// CHECK: @_PROTOCOLS__TtC14objc_protocols17ImplementingClass {{.*}} @_PROTOCOL__TtP14objc_protocols12BaseProtocol_
class ImplementingClass : InheritingProtocol { }

// CHECK-linux: @_PROTOCOL_PROTOCOLS_NSDoubleInheritedFunging = weak hidden constant{{.*}}i64 2{{.*}} @_PROTOCOL_NSFungingAndRuncing {{.*}}@_PROTOCOL_NSFunging
// CHECK-macosx: @"_OBJC_$_PROTOCOL_REFS_NSDoubleInheritedFunging" = internal global{{.*}}i64 2{{.*}} @"_OBJC_PROTOCOL_$_NSFungingAndRuncing"{{.*}} @"_OBJC_PROTOCOL_$_NSFunging"
// CHECK-linux-OLD: @_PROTOCOL_PROTOCOLS_NSDoubleInheritedFunging = weak hidden constant{{.*}}i64 2{{.*}} @_PROTOCOL_NSFungingAndRuncing {{.*}}@_PROTOCOL_NSFunging
// CHECK-macosx-OLD: @_PROTOCOL_PROTOCOLS_NSDoubleInheritedFunging = weak hidden constant{{.*}} @_PROTOCOL_NSFungingAndRuncing

// -- Force generation of witness for Zim.
// CHECK: define hidden swiftcc { %objc_object*, i8** } @"$s14objc_protocols22mixed_heritage_erasure{{[_0-9a-zA-Z]*}}F"
func mixed_heritage_erasure(_ x: Zim) -> Frungible {
  return x
  // CHECK: [[T0:%.*]] = insertvalue { %objc_object*, i8** } undef, %objc_object* {{%.*}}, 0
  // CHECK: insertvalue { %objc_object*, i8** } [[T0]], i8** getelementptr inbounds ([2 x i8*], [2 x i8*]* [[ZIM_FRUNGIBLE_WITNESS]], i32 0, i32 0), 1
}

// CHECK-OLD-LABEL: define hidden swiftcc void @"$s14objc_protocols0A8_generic{{[_0-9a-zA-Z]*}}F"(%objc_object* %0, %swift.type* %T) {{.*}} {
// CHECK-LABEL: define hidden swiftcc void @"$s14objc_protocols0A8_generic{{[_0-9a-zA-Z]*}}F"(%objc_object* %0, %swift.type* %T) {{.*}} {
func objc_generic<T : NSRuncing>(_ x: T) {
  x.runce()
  // CHECK: [[SELECTOR:%.*]] = load i8*, i8** @"\01L_selector(runce)", align 8
  // CHECK: bitcast %objc_object* %0 to [[OBJTYPE:.*]]*
  // CHECK: call void bitcast (void ()* @objc_msgSend to void ([[OBJTYPE]]*, i8*)*)([[OBJTYPE]]* {{%.*}}, i8* [[SELECTOR]])
}

// CHECK-LABEL: define hidden swiftcc void @"$s14objc_protocols05call_A8_generic{{[_0-9a-zA-Z]*}}F"(%objc_object* %0, %swift.type* %T) {{.*}} {
// CHECK:         call swiftcc void @"$s14objc_protocols0A8_generic{{[_0-9a-zA-Z]*}}F"(%objc_object* %0, %swift.type* %T)
func call_objc_generic<T : NSRuncing>(_ x: T) {
  objc_generic(x)
}

// CHECK-LABEL: define hidden swiftcc void @"$s14objc_protocols0A9_protocol{{[_0-9a-zA-Z]*}}F"(%objc_object* %0) {{.*}} {
func objc_protocol(_ x: NSRuncing) {
  x.runce()
  // CHECK: [[SELECTOR:%.*]] = load i8*, i8** @"\01L_selector(runce)", align 8
  // CHECK: bitcast %objc_object* %0 to [[OBJTYPE:.*]]*
  // CHECK: call void bitcast (void ()* @objc_msgSend to void ([[OBJTYPE]]*, i8*)*)([[OBJTYPE]]* {{%.*}}, i8* [[SELECTOR]])
}

// CHECK: define hidden swiftcc %objc_object* @"$s14objc_protocols0A8_erasure{{[_0-9a-zA-Z]*}}F"(%TSo7NSSpoonC* %0) {{.*}} {
func objc_erasure(_ x: NSSpoon) -> NSRuncing {
  return x
  // CHECK: [[RES:%.*]] = bitcast %TSo7NSSpoonC* {{%.*}} to %objc_object*
  // CHECK: ret %objc_object* [[RES]]
}

// CHECK: define hidden swiftcc void @"$s14objc_protocols0A21_protocol_composition{{[_0-9a-zA-Z]*}}F"(%objc_object* %0)
func objc_protocol_composition(_ x: NSRuncing & NSFunging) {
  x.runce()
  // CHECK: [[RUNCE:%.*]] = load i8*, i8** @"\01L_selector(runce)", align 8
  // CHECK: bitcast %objc_object* %0 to [[OBJTYPE:.*]]*
  // CHECK: call void bitcast (void ()* @objc_msgSend to void ([[OBJTYPE]]*, i8*)*)([[OBJTYPE]]* {{%.*}}, i8* [[RUNCE]])
  x.funge()
  // CHECK: [[FUNGE:%.*]] = load i8*, i8** @"\01L_selector(funge)", align 8
  // CHECK: call void bitcast (void ()* @objc_msgSend to void ([[OBJTYPE]]*, i8*)*)([[OBJTYPE]]* {{%.*}}, i8* [[FUNGE]])
}

// CHECK: define hidden swiftcc void @"$s14objc_protocols0A27_swift_protocol_composition{{[_0-9a-zA-Z]*}}F"(%objc_object* %0, i8** %1)
func objc_swift_protocol_composition
(_ x: NSRuncing & Ansible & NSFunging) {
  x.runce()
  // CHECK: [[RUNCE:%.*]] = load i8*, i8** @"\01L_selector(runce)", align 8
  // CHECK: bitcast %objc_object* %0 to [[OBJTYPE:.*]]*
  // CHECK: call void bitcast (void ()* @objc_msgSend to void ([[OBJTYPE]]*, i8*)*)([[OBJTYPE]]* {{%.*}}, i8* [[RUNCE]])
  /* TODO: Abstraction difference from ObjC protocol composition to 
   * opaque protocol
  x.anse()
   */
  x.funge()
  // CHECK: [[FUNGE:%.*]] = load i8*, i8** @"\01L_selector(funge)", align 8
  // CHECK: call void bitcast (void ()* @objc_msgSend to void ([[OBJTYPE]]*, i8*)*)([[OBJTYPE]]* {{%.*}}, i8* [[FUNGE]])
}

// TODO: Mixed class-bounded/fully general protocol compositions.

@objc protocol SettableProperty {
  var reqt: NSRuncing { get set }
}

func instantiateArchetype<T: SettableProperty>(_ x: T) {
  let y = x.reqt
  x.reqt = y
}

// rdar://problem/21029254

@objc protocol Appaloosa { }

protocol Palomino {}
protocol Vanner : Palomino, Appaloosa { }

struct Stirrup<T : Palomino> { }

func canter<T : Palomino>(_ t: Stirrup<T>) {}

func gallop<T : Vanner>(_ t: Stirrup<T>) {
  canter(t)
}

// https://github.com/apple/swift/issues/49678
func triggerDoubleInheritedFunging() -> AnyObject {
  return NSDoubleInheritedFunging.self as AnyObject
}

class TestNonRuntimeProto : RuntimeP { }

class TestNonRuntime2Proto : Runtime2P {}

public class SomeImpl : DeclarationOnlyUser {
    public func printIt() {
        print("hello")
    }
}
