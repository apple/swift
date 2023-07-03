
// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -module-name objc -primary-file %s -emit-ir -disable-objc-attr-requires-foundation-module | %FileCheck %s

// REQUIRES: CPU=x86_64
// REQUIRES: objc_interop

import Foundation
import gizmo

// CHECK: [[TYPE:%swift.type]] = type
// CHECK: [[ID:%T4objc2idV]] = type <{ %AnyObject }>
// CHECK: [[RECT:%TSo4RectV]] = type
// CHECK: [[FLOAT:%TSf]] = type

// CHECK: @"\01L_selector_data(bar)" = private global [4 x i8] c"bar\00", section "__TEXT,__objc_methname,cstring_literals", align 1
// CHECK: @"\01L_selector(bar)" = private externally_initialized global ptr @"\01L_selector_data(bar)", section "__DATA,__objc_selrefs,literal_pointers,no_dead_strip", align 8

// CHECK: @"\01L_selector_data(acquiesce)"
// CHECK-NOT: @"\01L_selector_data(disharmonize)"
// CHECK: @"\01L_selector_data(eviscerate)"

// CHECK: @"$sSo4RectVMn" = linkonce_odr hidden constant
// CHECK: @"$sSo4RectVMf" = linkonce_odr hidden constant

struct id {
  var data : AnyObject
}

// Exporting something as [objc] doesn't make it an ObjC class.
@objc class Blammo {
}
// Class and methods are [objc] by inheritance.
class MyBlammo : Blammo {
  func foo() {}
// CHECK:  define hidden swiftcc void @"$s4objc8MyBlammoC3fooyyF"(ptr swiftself %0) {{.*}} {
// CHECK:    ret void
}

// Class and methods are [objc] by inheritance.
class Test2 : Gizmo {
  func foo() {}
// CHECK:  define hidden swiftcc void @"$s4objc5Test2C3fooyyF"(ptr swiftself %0) {{.*}} {
// CHECK:    ret void

  @objc dynamic func bar() {}
}

// Test @nonobjc.
class Contrarian : Blammo {
  @objc func acquiesce() {}
  @nonobjc func disharmonize() {}
  @nonobjc func eviscerate() {}
}

class Octogenarian : Contrarian {
  // Override of @nonobjc is @objc again unless made @nonobjc.
  @nonobjc override func disharmonize() {}

  // Override of @nonobjc can be @objc.
  @objc override func eviscerate() {}
}

@_silgen_name("unknown")
func unknown(_ x: id) -> id

// CHECK:    define hidden swiftcc ptr @"$s4objc5test0{{[_0-9a-zA-Z]*}}F"(ptr %0)
// CHECK-NOT:  call {{.*}} @swift_unknownObjectRetain
// CHECK:      call {{.*}} @swift_unknownObjectRetain
// CHECK-NOT:  call {{.*}} @swift_unknownObjectRelease
// CHECK:      call {{.*}} @swift_unknownObjectRelease
// CHECK:      ret ptr
func test0(_ arg: id) -> id {
  var x : id
  x = arg
  unknown(x)
  var y = x
  return y
}

func test1(_ cell: Blammo) {}
// CHECK:  define hidden swiftcc void @"$s4objc5test1{{[_0-9a-zA-Z]*}}F"(ptr %0) {{.*}} {
// CHECK-NEXT:    entry
// CHECK-NEXT:    alloca
// CHECK-NEXT:    memset
// CHECK-NEXT:    store
// CHECK-NEXT:    ret void


// FIXME: These ownership convention tests should become SILGen tests.
func test2(_ v: Test2) { v.bar() }
func test3() -> NSObject {
  return Gizmo()
}
// Normal message send with argument, no transfers.
func test5(_ g: Gizmo) {
  Gizmo.inspect(g)
}
// The argument to consume: is __attribute__((ns_consumed)).
func test6(_ g: Gizmo) {
  Gizmo.consume(g)
}
// fork is __attribute__((ns_consumes_self)).
func test7(_ g: Gizmo) {
  g.fork()
}
// clone is __attribute__((ns_returns_retained)).
func test8(_ g: Gizmo) {
  g.clone()
}
// duplicate has an object returned at +0.
func test9(_ g: Gizmo) {
  g.duplicate()
}

func test10(_ g: Gizmo, r: Rect) {
  Gizmo.run(with: r, andGizmo:g);
}

// Force the emission of the Rect metadata.
func test11_helper<T>(_ t: T) {}
// NSRect's metadata needs to be uniqued at runtime using getForeignTypeMetadata.
// CHECK-LABEL: define hidden swiftcc void @"$s4objc6test11yySo4RectVF"
// CHECK:         call swiftcc %swift.metadata_response @swift_getForeignTypeMetadata(i64 %0, {{.*}} @"$sSo4RectVMf"
func test11(_ r: Rect) { test11_helper(r) }

class WeakObjC {
  weak var obj: NSObject?
  weak var id: AnyObject?

  init() {
    var foo = obj
    var bar: AnyObject? = id
  }
}

// rdar://17528908
// CHECK:  i32 1, !"Objective-C Version", i32 2}
// CHECK:  i32 1, !"Objective-C Image Info Version", i32 0}
// CHECK:  i32 1, !"Objective-C Image Info Section", !"__DATA,__objc_imageinfo,regular,no_dead_strip"}
//   84477696 == (5 << 24) | (9 << 16) | (7 << 8).
//     5 and 8 is the current major.minor version. 7 is the Swift ABI version.
// CHECK:  i32 4, !"Objective-C Garbage Collection", i32 84477696}
// CHECK:  i32 1, !"Swift Version", i32 7}
