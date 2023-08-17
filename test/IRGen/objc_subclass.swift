// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/Inputs -I %t) -primary-file %s -emit-ir | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize %s

// UNSUPPORTED: OS=watchos

// REQUIRES: objc_interop

// The order of the output seems to change between asserts/noasserts build of
// the stlib.
// REQUIRES: swift_stdlib_asserts

// CHECK: [[OBJC_CLASS:%objc_class]] = type
// CHECK: [[OPAQUE:%swift.opaque]] = type
// CHECK: [[INT:%TSi]] = type <{ [[LLVM_PTRSIZE_INT:i(32|64)]] }>

// CHECK-32: @"$s13objc_subclass10SwiftGizmoC1xSivpWvd" = hidden global i32 4, align [[WORD_SIZE_IN_BYTES:4]]
// CHECK-64: @"$s13objc_subclass10SwiftGizmoC1xSivpWvd" = hidden global i64 8, align [[WORD_SIZE_IN_BYTES:8]]

// CHECK: @"OBJC_METACLASS_$__TtC13objc_subclass10SwiftGizmo" = hidden global [[OBJC_CLASS]] { ptr @"OBJC_METACLASS_$_NSObject", ptr @"OBJC_METACLASS_$_Gizmo", ptr @_objc_empty_cache, ptr null, [[LLVM_PTRSIZE_INT]] ptrtoint ({{.*}} @_METACLASS_DATA__TtC13objc_subclass10SwiftGizmo to [[LLVM_PTRSIZE_INT]]) }

// CHECK-32: @_METACLASS_DATA__TtC13objc_subclass10SwiftGizmo = internal constant { {{.*}}ptr } {
// CHECK-32:   i32 129,
// CHECK-32:   i32 20,
// CHECK-32:   i32 20,
// CHECK-32:   ptr null,
// CHECK-32:   ptr @.str.31._TtC13objc_subclass10SwiftGizmo,
// CHECK-32:   ptr null,
// CHECK-32:   ptr null,
// CHECK-32:   ptr null,
// CHECK-32:   ptr null,
// CHECK-32:   ptr null
// CHECK-32: }, section "__DATA, {{.*}}", align 4

// CHECK-64: @_METACLASS_DATA__TtC13objc_subclass10SwiftGizmo = internal constant { {{.*}}ptr } {
// CHECK-64:   i32 129,
// CHECK-64:   i32 40,
// CHECK-64:   i32 40,
// CHECK-64:   i32 0,
// CHECK-64:   ptr null,
// CHECK-64:   ptr @.str.31._TtC13objc_subclass10SwiftGizmo,
// CHECK-64:   ptr null,
// CHECK-64:   ptr null,
// CHECK-64:   ptr null,
// CHECK-64:   ptr null,
// CHECK-64:   ptr null
// CHECK-64: }, section "__DATA, {{.*}}", align 8

// CHECK-32: @_INSTANCE_METHODS__TtC13objc_subclass10SwiftGizmo = internal constant { {{.*}}] } {
// CHECK-32:   i32 12,
// CHECK-32:   i32 11,
// CHECK-32:   [11 x { ptr, ptr, ptr }] [{ ptr, ptr, ptr } {
// CHECK-32:     ptr @"\01L_selector_data(x)",
// CHECK-32:     ptr @".str.6.l8@0:4",
// CHECK-32:     ptr @"$s13objc_subclass10SwiftGizmoC1xSivgTo{{(\.ptrauth)?}}"
// CHECK-32:   }, { ptr, ptr, ptr } {
// CHECK-32:     ptr @"\01L_selector_data(setX:)",
// CHECK-32:     ptr @".str.9.v12@0:4l8",
// CHECK-32:     ptr @"$s13objc_subclass10SwiftGizmoC1xSivsTo{{(\.ptrauth)?}}"
// CHECK-32:   }, { ptr, ptr, ptr } {
// CHECK-32:     ptr @"\01L_selector_data(getX)",
// CHECK-32:     ptr @".str.6.l8@0:4",
// CHECK-32:     ptr @"$s13objc_subclass10SwiftGizmoC4getXSiyFTo{{(\.ptrauth)?}}"
// CHECK-32:   }, { ptr, ptr, ptr } {
// CHECK-32:     ptr @"\01L_selector_data(duplicate)",
// CHECK-32:     ptr @".str.6.@8@0:4",
// CHECK-32:     ptr @"$s13objc_subclass10SwiftGizmoC9duplicateSo0D0CyFTo{{(\.ptrauth)?}}"
// CHECK-32:   }, { ptr, ptr, ptr } {
// CHECK-32:     ptr @"\01L_selector_data(init)",
// CHECK-32:     ptr @".str.6.@8@0:4",
// CHECK-32:     ptr @"$s13objc_subclass10SwiftGizmoCACycfcTo{{(\.ptrauth)?}}"
// CHECK-32:   }, { ptr, ptr, ptr } {
// CHECK-32:     ptr @"\01L_selector_data(initWithInt:string:)",
// CHECK-32:     ptr @".str.12.@16@0:4l8@12",
// CHECK-32:     ptr @"$s13objc_subclass10SwiftGizmoC3int6stringACSi_SStcfcTo{{(\.ptrauth)?}}"
// CHECK-32:   }, { ptr, ptr, ptr } {
// CHECK-32:     ptr @"\01L_selector_data(dealloc)",
// CHECK-32:     ptr @".str.6.v8@0:4",
// CHECK-32:     ptr @"$s13objc_subclass10SwiftGizmoCfDTo{{(\.ptrauth)?}}"
// CHECK-32:   }, { ptr, ptr, ptr } {
// CHECK-32:     ptr @"\01L_selector_data(isEnabled)",
// CHECK-32:     ptr {{@".str.[^"]+"}},
// CHECK-32:     ptr @"$s13objc_subclass10SwiftGizmoC7enabledSbvgTo{{(\.ptrauth)?}}"
// CHECK-32:   }, { ptr, ptr, ptr } {
// CHECK-32:     ptr @"\01L_selector_data(setIsEnabled:)",
// CHECK-32:     ptr {{@".str.[^"]+"}},
// CHECK-32:     ptr @"$s13objc_subclass10SwiftGizmoC7enabledSbvsTo{{(\.ptrauth)?}}"
// CHECK-32:   }, { ptr, ptr, ptr } {
// CHECK-32:     ptr @"\01L_selector_data(initWithBellsOn:)",
// CHECK-32:     ptr {{@".str.[^"]+"}},
// CHECK-32:     ptr @"$s13objc_subclass10SwiftGizmoC7bellsOnACSgSi_tcfcTo{{(\.ptrauth)?}}"
// CHECK-32:   }, { ptr, ptr, ptr } {
// CHECK-32:     ptr @"\01L_selector_data(.cxx_construct)",
// CHECK-32:     ptr {{@".str.[^"]+"}},
// CHECK-32:     ptr @"$s13objc_subclass10SwiftGizmoCfeTo{{(\.ptrauth)?}}"
// CHECK-32:   }]
// CHECK-32: }, section "__DATA, {{.*}}", align 4

// CHECK-64: @_INSTANCE_METHODS__TtC13objc_subclass10SwiftGizmo = internal constant { {{.*}}] } {
// CHECK-64:   i32 24,
// CHECK-64:   i32 11,
// CHECK-64:   [11 x { ptr, ptr, ptr }] [{
// CHECK-64:      ptr @"\01L_selector_data(x)",
// CHECK-64:      ptr @".str.7.q16@0:8"
// CHECK-64:      ptr @"$s13objc_subclass10SwiftGizmoC1xSivgTo{{(\.ptrauth)?}}"
// CHECK-64:   }, {
// CHECK-64:      ptr @"\01L_selector_data(setX:)",
// CHECK-64:      ptr @".str.10.v24@0:8q16"
// CHECK-64:      ptr @"$s13objc_subclass10SwiftGizmoC1xSivsTo{{(\.ptrauth)?}}"
// CHECK-64:   }, {
// CHECK-64:      ptr @"\01L_selector_data(getX)",
// CHECK-64:      ptr @".str.7.q16@0:8"
// CHECK-64:      ptr @"$s13objc_subclass10SwiftGizmoC4getXSiyFTo{{(\.ptrauth)?}}"
// CHECK-64:   }, {
// CHECK-64:     ptr @"\01L_selector_data(duplicate)",
// CHECK-64:     ptr @".str.7.@16@0:8",
// CHECK-64:     ptr @"$s13objc_subclass10SwiftGizmoC9duplicateSo0D0CyFTo{{(\.ptrauth)?}}"
// CHECK-64:   }, {
// CHECK-64:     ptr @"\01L_selector_data(init)",
// CHECK-64:     ptr @".str.7.@16@0:8",
// CHECK-64:     ptr @"$s13objc_subclass10SwiftGizmoCACycfcTo{{(\.ptrauth)?}}"
// CHECK-64:   }, {
// CHECK-64:     ptr @"\01L_selector_data(initWithInt:string:)",
// CHECK-64:     ptr @".str.13.@32@0:8q16@24",
// CHECK-64:     ptr @"$s13objc_subclass10SwiftGizmoC3int6stringACSi_SStcfcTo{{(\.ptrauth)?}}"
// CHECK-64:   }, {
// CHECK-64:     ptr @"\01L_selector_data(dealloc)",
// CHECK-64:     ptr @".str.7.v16@0:8",
// CHECK-64:     ptr @"$s13objc_subclass10SwiftGizmoCfDTo{{(\.ptrauth)?}}"
// CHECK-64:   }, {
// CHECK-64:     ptr @"\01L_selector_data(isEnabled)",
// CHECK-64:     ptr @".str.7.{{[Bc]}}16@0:8",
// CHECK-64:     ptr @"$s13objc_subclass10SwiftGizmoC7enabledSbvgTo{{(\.ptrauth)?}}"
// CHECK-64:   }, {
// CHECK-64:     ptr @"\01L_selector_data(setIsEnabled:)",
// CHECK-64:     ptr @".str.10.v20@0:8{{[Bc]}}16",
// CHECK-64:     ptr @"$s13objc_subclass10SwiftGizmoC7enabledSbvsTo{{(\.ptrauth)?}}"
// CHECK-64:   }, {
// CHECK-64:     ptr @"\01L_selector_data(initWithBellsOn:)",
// CHECK-64:     ptr @".str.10.@24@0:8q16",
// CHECK-64:     ptr @"$s13objc_subclass10SwiftGizmoC7bellsOnACSgSi_tcfcTo{{(\.ptrauth)?}}"
// CHECK-64:   }, {
// CHECK-64:     ptr @"\01L_selector_data(.cxx_construct)",
// CHECK-64:     ptr @".str.7.v16@0:8",
// CHECK-64:     ptr @"$s13objc_subclass10SwiftGizmoCfeTo{{(\.ptrauth)?}}"
// CHECK-64:   }]
// CHECK-64: }, section "__DATA, {{.*}}", align 8

// CHECK-32: @_IVARS__TtC13objc_subclass10SwiftGizmo = internal constant { {{.*}}] } {
// CHECK-32:   i32 20,
// CHECK-32:   i32 1,
// CHECK-32:   [1 x { ptr, ptr, ptr, i32, i32 }] [{ ptr, ptr, ptr, i32, i32 } {
// CHECK-32:     ptr @"$s13objc_subclass10SwiftGizmoC1xSivpWvd",
// CHECK-32:     ptr @.str.1.x,
// CHECK-32:     ptr {{.*}},
// CHECK-32:     i32 2,
// CHECK-32:     i32 4 }]
// CHECK-32: }, section "__DATA, {{.*}}", align 4

// CHECK-64: @_IVARS__TtC13objc_subclass10SwiftGizmo = internal constant { {{.*}}] } {
// CHECK-64:   i32 32,
// CHECK-64:   i32 1,
// CHECK-64:   [1 x { ptr, ptr, ptr, i32, i32 }] [{ ptr, ptr, ptr, i32, i32 } {
// CHECK-64:     ptr @"$s13objc_subclass10SwiftGizmoC1xSivpWvd",
// CHECK-64:     ptr @.str.1.x,
// CHECK-64:     ptr @.str.0.,
// CHECK-64:     i32 3,
// CHECK-64:     i32 8 }]
// CHECK-64: }, section "__DATA, {{.*}}", align 8

// CHECK-32: @_DATA__TtC13objc_subclass10SwiftGizmo = internal constant { {{.*}}ptr } {
// CHECK-32:   i32 132,
// CHECK-32:   i32 4,
// CHECK-32:   i32 8,
// CHECK-32:   ptr null,
// CHECK-32:   ptr @.str.31._TtC13objc_subclass10SwiftGizmo,
// CHECK-32:   @_INSTANCE_METHODS__TtC13objc_subclass10SwiftGizmo,
// CHECK-32:   ptr null,
// CHECK-32:   @_IVARS__TtC13objc_subclass10SwiftGizmo,
// CHECK-32:   ptr null,
// CHECK-32:   @_PROPERTIES__TtC13objc_subclass10SwiftGizmo
// CHECK-32: }, section "__DATA, {{.*}}", align 4

// CHECK-64: @_DATA__TtC13objc_subclass10SwiftGizmo = internal constant { {{.*}}ptr } {
// CHECK-64:    i32 132,
// CHECK-64:    i32 8,
// CHECK-64:    i32 16,
// CHECK-64:    i32 0,
// CHECK-64:    ptr null,
// CHECK-64:    ptr @.str.31._TtC13objc_subclass10SwiftGizmo,
// CHECK-64:    @_INSTANCE_METHODS__TtC13objc_subclass10SwiftGizmo,
// CHECK-64:    ptr null,
// CHECK-64:    @_IVARS__TtC13objc_subclass10SwiftGizmo,
// CHECK-64:    ptr null,
// CHECK-64:    @_PROPERTIES__TtC13objc_subclass10SwiftGizmo
// CHECK-64:  }, section "__DATA, {{.*}}", align 8

// CHECK-NOT: @_TMCSo13SwiftGizmo = {{.*NSObject}}

// CHECK: @_INSTANCE_METHODS__TtC13objc_subclass12GenericGizmo

// CHECK-32: [[SETTER_ENCODING:@.*]] = private unnamed_addr constant [10 x i8] c"v12@0:4@8\00"
// CHECK-64: [[SETTER_ENCODING:@.*]] = private unnamed_addr constant [11 x i8] c"v24@0:8@16\00"

// CHECK-32: @_INSTANCE_METHODS__TtC13objc_subclass11SwiftGizmo2 = internal constant { i32, i32, [5 x { ptr, ptr, ptr }] } {
// CHECK-32:   i32 12,
// CHECK-32:   i32 5,
// CHECK-32:   [5 x { ptr, ptr, ptr }] [
// CHECK-32:     {
// CHECK-32:       ptr @"\01L_selector_data(sg)",
// CHECK-32:       ptr @".str.6.@8@0:4",
// CHECK-32:       ptr @"$s13objc_subclass11SwiftGizmo2C2sgAA0C5GizmoCvgTo{{(\.ptrauth)?}}"
// CHECK-32:     }, {
// CHECK-32:       ptr @"\01L_selector_data(setSg:)",
// CHECK-32:       ptr [[SETTER_ENCODING]],
// CHECK-32:       ptr @"$s13objc_subclass11SwiftGizmo2C2sgAA0C5GizmoCvsTo{{(\.ptrauth)?}}"
// CHECK-32:     }, {
// CHECK-32:       ptr @"\01L_selector_data(init)",
// CHECK-32:       ptr @".str.6.@8@0:4",
// CHECK-32:       ptr @"$s13objc_subclass11SwiftGizmo2CACycfcTo{{(\.ptrauth)?}}"
// CHECK-32:     }, {
// CHECK-32:       ptr @"\01L_selector_data(initWithBellsOn:)",
// CHECK-32:       ptr {{@".str.[^"]+"}},
// CHECK-32:       ptr @"$s13objc_subclass11SwiftGizmo2C7bellsOnACSgSi_tcfcTo{{(\.ptrauth)?}}"
// CHECK-32:     }, {
// CHECK-32:       ptr @"\01L_selector_data(.cxx_destruct)",
// CHECK-32:       ptr {{@".str.[^"]+"}},
// CHECK-32:       ptr @"$s13objc_subclass11SwiftGizmo2CfETo{{(\.ptrauth)?}}"
// CHECK-32:     }
// CHECK-32:   ]
// CHECK-32: }, section "__DATA, {{.*}}", align 4

// CHECK-64: @_INSTANCE_METHODS__TtC13objc_subclass11SwiftGizmo2 = internal constant { i32, {{.*}}] } {
// CHECK-64:   i32 24,
// CHECK-64:   i32 5,
// CHECK-64:   [5 x { ptr, ptr, ptr }] [
// CHECK-64:     {
// CHECK-64:       ptr @"\01L_selector_data(sg)",
// CHECK-64:       ptr @".str.7.@16@0:8",
// CHECK-64:       ptr @"$s13objc_subclass11SwiftGizmo2C2sgAA0C5GizmoCvgTo{{(\.ptrauth)?}}"
// CHECK-64:     }, {
// CHECK-64:       ptr @"\01L_selector_data(setSg:)",
// CHECK-64:       ptr [[SETTER_ENCODING]],
// CHECK-64:       ptr @"$s13objc_subclass11SwiftGizmo2C2sgAA0C5GizmoCvsTo{{(\.ptrauth)?}}"
// CHECK-64:     }, {
// CHECK-64:       ptr @"\01L_selector_data(init)",
// CHECK-64:       ptr @".str.7.@16@0:8",
// CHECK-64:       ptr @"$s13objc_subclass11SwiftGizmo2CACycfcTo{{(\.ptrauth)?}}"
// CHECK-64:     }, {
// CHECK-64:       ptr @"\01L_selector_data(initWithBellsOn:)",
// CHECK-64:       ptr @".str.10.@24@0:8q16",
// CHECK-64:       ptr @"$s13objc_subclass11SwiftGizmo2C7bellsOnACSgSi_tcfcTo{{(\.ptrauth)?}}"
// CHECK-64:     }, {
// CHECK-64:       ptr @"\01L_selector_data(.cxx_destruct)",
// CHECK-64:       ptr @".str.7.v16@0:8",
// CHECK-64:       ptr @"$s13objc_subclass11SwiftGizmo2CfETo{{(\.ptrauth)?}}"
// CHECK-64:     }
// CHECK-64: ] }


// CHECK: @"objc_classes_$s13objc_subclass10SwiftGizmoCN" = internal global ptr @"$s13objc_subclass10SwiftGizmoCN", section "__DATA,__objc_classlist,regular,no_dead_strip", no_sanitize_address, align [[WORD_SIZE_IN_BYTES]]
// CHECK: @"objc_classes_$s13objc_subclass11SwiftGizmo2CN" = internal global ptr @"$s13objc_subclass11SwiftGizmo2CN", section "__DATA,__objc_classlist,regular,no_dead_strip", no_sanitize_address, align [[WORD_SIZE_IN_BYTES]]

// CHECK: @objc_non_lazy_classes = internal global [1 x ptr] [ptr @"$s13objc_subclass11SwiftGizmo2CN"], section "__DATA,__objc_nlclslist,regular,no_dead_strip", no_sanitize_address, align [[WORD_SIZE_IN_BYTES]]

import Foundation
import gizmo

@requires_stored_property_inits
class SwiftGizmo : Gizmo {
  @objc var x = Int()

  @objc func getX() -> Int {
    return x
  }

  override func duplicate() -> Gizmo {
    return SwiftGizmo()
  }

  override init() {
    super.init(bellsOn:0)
  }

  @objc init(int i: Int, string str : String) {
    super.init(bellsOn:i)
  }

  deinit { var x = 10 }

  @objc var enabled: Bool {
    @objc(isEnabled) get {
      return true
    }

    @objc(setIsEnabled:) set {
    }
  }
}

class GenericGizmo<T> : Gizmo {
  @objc func foo() {}

  @objc var x : Int {
    return 0
  }

  var array : [T] = []
}
// CHECK: define hidden swiftcc [[LLVM_PTRSIZE_INT]] @"$s13objc_subclass12GenericGizmoC1xSivg"(

var sg = SwiftGizmo()
sg.duplicate()

@_objc_non_lazy_realization
class SwiftGizmo2 : Gizmo {
  @objc var sg : SwiftGizmo

  override init() {
    sg = SwiftGizmo()
    super.init()
  }

  deinit { }
}

