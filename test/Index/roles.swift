// RUN: %empty-directory(%t)
//
// RUN: %target-swift-frontend -emit-module -o %t %S/Inputs/imported_swift_module.swift
// RUN: %target-swift-ide-test -print-indexed-symbols -source-filename %s -I %t | %FileCheck %s

import func imported_swift_module.importedFunc
// CHECK: [[@LINE-1]]:35 | function/Swift | importedFunc() | s:21imported_swift_module0A4FuncyyF | Ref | rel: 0
import var imported_swift_module.importedGlobal
// CHECK: [[@LINE-1]]:34 | variable/Swift | importedGlobal | s:21imported_swift_module0A6GlobalSivp | Ref | rel: 0

// Definition
let x = 2
// CHECK: [[@LINE-1]]:5 | variable/Swift | x | s:14swift_ide_test1xSivp | Def | rel: 0

// Definition + Read of x
var y = x + 1
// CHECK: [[@LINE-1]]:5 | variable/Swift | y | s:14swift_ide_test1ySivp | Def | rel: 0
// CHECK: [[@LINE-2]]:9 | variable/Swift | x | s:14swift_ide_test1xSivp | Ref,Read,RelCont | rel: 1
// CHECK: [[@LINE-3]]:11 | static-method/infix-operator/Swift | +(_:_:) | s:Si1poiyS2i_SitFZ | Ref,Call,RelCont | rel: 1
// CHECK-NEXT: RelCont | variable/Swift | y | s:14swift_ide_test1ySivp

// Read of x + Write of y
y = x + 1
// CHECK: [[@LINE-1]]:1 | variable/Swift | y | s:14swift_ide_test1ySivp | Ref,Writ | rel: 0
// CHECK: [[@LINE-2]]:5 | variable/Swift | x | s:14swift_ide_test1xSivp | Ref,Read | rel: 0
// CHECK: [[@LINE-3]]:7 | static-method/infix-operator/Swift | +(_:_:) | s:Si1poiyS2i_SitFZ | Ref,Call | rel: 0


// Read of y + Write of y
y += x
// CHECK: [[@LINE-1]]:1 | variable/Swift | y | s:14swift_ide_test1ySivp | Ref,Read,Writ | rel: 0
// CHECK: [[@LINE-2]]:3 | static-method/infix-operator/Swift | +=(_:_:) | s:Si2peoiyySiz_SitFZ | Ref,Call | rel: 0
// CHECK: [[@LINE-3]]:6 | variable/Swift | x | s:14swift_ide_test1xSivp | Ref,Read | rel: 0

var z: Int {
// CHECK: [[@LINE-1]]:5 | variable/Swift | z | s:14swift_ide_test1zSivp | Def | rel: 0
  get {
    // CHECK: [[@LINE-1]]:3 | function/acc-get/Swift | getter:z | s:14swift_ide_test1zSivg | Def,RelChild,RelAcc | rel: 1

    return y
    // CHECK: [[@LINE-1]]:12 | variable/Swift | y | s:14swift_ide_test1ySivp | Ref,Read,RelCont | rel: 1
    // CHECK-NEXT: RelCont | function/acc-get/Swift | getter:z | s:14swift_ide_test1zSivg
  }
  set {
    // CHECK: [[@LINE-1]]:3 | function/acc-set/Swift | setter:z | s:14swift_ide_test1zSivs | Def,RelChild,RelAcc | rel: 1

    y = newValue
    // CHECK: [[@LINE-1]]:5 | variable/Swift | y | s:14swift_ide_test1ySivp | Ref,Writ,RelCont | rel: 1
    // CHECK-NEXT: RelCont | function/acc-set/Swift | setter:z | s:14swift_ide_test1zSivs
  }
}
// Write + Read of z
z = z + 1
// CHECK: [[@LINE-1]]:1 | variable/Swift | z | s:14swift_ide_test1zSivp | Ref,Writ | rel: 0
// CHECK: [[@LINE-2]]:1 | function/acc-set/Swift | setter:z | s:14swift_ide_test1zSivs | Ref,Call,Impl | rel: 0
// CHECK: [[@LINE-3]]:5 | variable/Swift | z | s:14swift_ide_test1zSivp | Ref,Read | rel: 0
// CHECK: [[@LINE-4]]:5 | function/acc-get/Swift | getter:z | s:14swift_ide_test1zSivg | Ref,Call,Impl | rel: 0

// Call
func aCalledFunction(a: Int, b: inout Int) {
// CHECK: [[@LINE-1]]:6 | function/Swift | aCalledFunction(a:b:) | s:14swift_ide_test15aCalledFunction1a1bySi_SiztF | Def | rel: 0
// CHECK: [[@LINE-2]]:22 | param/Swift | a | s:{{.*}} | Def,RelChild | rel: 1
// CHECK-NEXT: RelChild | function/Swift | aCalledFunction(a:b:) | s:14swift_ide_test15aCalledFunction1a1bySi_SiztF
// CHECK: [[@LINE-4]]:30 | param/Swift | b | s:{{.*}} | Def,RelChild | rel: 1
// CHECK-NEXT: RelChild | function/Swift | aCalledFunction(a:b:) | s:14swift_ide_test15aCalledFunction1a1bySi_SiztF

  var _ = a + b
  // CHECK: [[@LINE-1]]:11 | param/Swift | a | s:{{.*}} | Ref,Read,RelCont | rel: 1
  // CHECK-NEXT: RelCont | function/Swift | aCalledFunction(a:b:) | s:14swift_ide_test15aCalledFunction1a1bySi_SiztF
  // CHECK: [[@LINE-3]]:15 | param/Swift | b | s:{{.*}} | Ref,Read,RelCont | rel: 1
  // CHECK-NEXT: RelCont | function/Swift | aCalledFunction(a:b:) | s:14swift_ide_test15aCalledFunction1a1bySi_SiztF

  b = a + 1
  // CHECK: [[@LINE-1]]:3 | param/Swift | b | s:{{.*}} | Ref,Writ,RelCont | rel: 1
  // CHECK-NEXT: RelCont | function/Swift | aCalledFunction(a:b:) | s:14swift_ide_test15aCalledFunction1a1bySi_SiztF
  // CHECK: [[@LINE-3]]:7 | param/Swift | a | s:{{.*}} | Ref,Read,RelCont | rel: 1
  // CHECK-NEXT: RelCont | function/Swift | aCalledFunction(a:b:) | s:14swift_ide_test15aCalledFunction1a1bySi_SiztF

  _ = { ignored in ignored + 1}
  // CHECK-NOT: [[@LINE-1]]:9 {{.*}} | ignored | {{.*}}

}

aCalledFunction(a: 1, b: &z)
// CHECK: [[@LINE-1]]:1 | function/Swift | aCalledFunction(a:b:) | s:14swift_ide_test15aCalledFunction1a1bySi_SiztF | Ref,Call | rel: 0
// CHECK: [[@LINE-2]]:27 | variable/Swift | z | s:14swift_ide_test1zSivp | Ref,Read,Writ | rel: 0

func aCaller() {
  // CHECK: [[@LINE-1]]:6 | function/Swift | aCaller() | s:14swift_ide_test7aCalleryyF | Def | rel: 0

  aCalledFunction(a: 1, b: &z)
  // CHECK: [[@LINE-1]]:3 | function/Swift | aCalledFunction(a:b:) | s:14swift_ide_test15aCalledFunction1a1bySi_SiztF | Ref,Call,RelCall,RelCont | rel: 1
  // CHECK-NEXT: RelCall,RelCont | function/Swift | aCaller() | s:14swift_ide_test7aCalleryyF
}

let aRef = aCalledFunction
// CHECK: [[@LINE-1]]:12 | function/Swift | aCalledFunction(a:b:) | s:14swift_ide_test15aCalledFunction1a1bySi_SiztF | Ref,RelCont | rel: 1

// RelationChildOf, Implicit
struct AStruct {
// CHECK: [[@LINE-1]]:8 | struct/Swift | AStruct | [[AStruct_USR:.*]] | Def | rel: 0

  let y: Int = 2
  // CHECK: [[@LINE-1]]:7 | instance-property/Swift | y | [[AStruct_y_USR:.*]] | Def,RelChild | rel: 1

  var x: Int
  // CHECK: [[@LINE-1]]:7 | instance-property/Swift | x | [[AStruct_x_USR:.*]] | Def,RelChild | rel: 1
  // CHECK-NEXT: RelChild | struct/Swift | AStruct | [[AStruct_USR]]

  mutating func aMethod() {
    // CHECK: [[@LINE-1]]:17 | instance-method/Swift | aMethod() | s:14swift_ide_test7AStructV7aMethodyyF | Def,RelChild | rel: 1
    // CHECK-NEXT: RelChild | struct/Swift | AStruct | [[AStruct_USR]]

    x += 1
    // CHECK: [[@LINE-1]]:5 | instance-property/Swift | x | [[AStruct_x_USR]] | Ref,Read,Writ,RelCont | rel: 1
    // CHECK-NEXT: RelCont | instance-method/Swift | aMethod() | s:14swift_ide_test7AStructV7aMethodyyF
    // CHECK: [[@LINE-3]]:5 | instance-method/acc-get/Swift | getter:x | s:14swift_ide_test7AStructV1xSivg | Ref,Call,Impl,RelCall,RelCont | rel: 1
    // CHECK-NEXT: RelCall,RelCont | instance-method/Swift | aMethod() | s:14swift_ide_test7AStructV7aMethodyyF
    // CHECK: [[@LINE-5]]:5 | instance-method/acc-set/Swift | setter:x | s:14swift_ide_test7AStructV1xSivs | Ref,Call,Impl,RelCall,RelCont | rel: 1
    // CHECK-NEXT: RelCall,RelCont | instance-method/Swift | aMethod() | s:14swift_ide_test7AStructV7aMethodyyF
    // CHECK: [[@LINE-7]]:7 | static-method/infix-operator/Swift | +=(_:_:) | s:Si2peoiyySiz_SitFZ | Ref,Call,RelCall,RelCont | rel: 1
    // CHECK-NEXT: RelCall,RelCont | instance-method/Swift | aMethod() | s:14swift_ide_test7AStructV7aMethodyyF
  }

  // RelationChildOf, RelationAccessorOf
  subscript(index: Int) -> Int {
    // CHECK: [[@LINE-1]]:3 | instance-property/subscript/Swift | subscript(_:) | s:14swift_ide_test7AStructVyS2icip | Def,RelChild | rel: 1
    // CHECK-NEXT: RelChild | struct/Swift | AStruct | [[AStruct_USR]]

    get {
      // CHECK: [[@LINE-1]]:5 | instance-method/acc-get/Swift | getter:subscript(_:) | s:14swift_ide_test7AStructVyS2icig | Def,RelChild,RelAcc | rel: 1
      // CHECK-NEXT: RelChild,RelAcc | instance-property/subscript/Swift | subscript(_:) | s:14swift_ide_test7AStructVyS2icip

      return x
    }
    set {
      // CHECK: [[@LINE-1]]:5 | instance-method/acc-set/Swift | setter:subscript(_:) | s:14swift_ide_test7AStructVyS2icis | Def,RelChild,RelAcc | rel: 1
      // CHECK-NEXT: RelChild,RelAcc | instance-property/subscript/Swift | subscript(_:) | s:14swift_ide_test7AStructVyS2ici

      x = newValue
    }
  }
}

class AClass {
// CHECK: [[@LINE-1]]:7 | class/Swift | AClass | [[AClass_USR:.*]] | Def | rel: 0
  var y: AStruct
  // CHECK: [[@LINE-1]]:7 | instance-property/Swift | y | [[AClass_y_USR:.*]] | Def,RelChild | rel: 1
  // CHECK: [[@LINE-2]]:7 | instance-method/acc-get/Swift | getter:y | [[AClass_y_get_USR:.*]] | Def,Dyn,Impl,RelChild,RelAcc | rel: 1
  // CHECK-NEXT:   RelChild,RelAcc | instance-property/Swift | y | [[AClass_y_USR]]
  // CHECK: [[@LINE-4]]:7 | instance-method/acc-set/Swift | setter:y | [[AClass_y_set_USR:.*]] | Def,Dyn,Impl,RelChild,RelAcc | rel: 1
  // CHECK-NEXT:   RelChild,RelAcc | instance-property/Swift | y | [[AClass_y_USR]]
  var z: [Int]
  var computed_p: Int { return 0 }
  // CHECK: [[@LINE-1]]:7 | instance-property/Swift | computed_p | [[AClass_computed_p_USR:.*]] | Def,RelChild | rel: 1
  // CHECK: [[@LINE-2]]:23 | instance-method/acc-get/Swift | getter:computed_p | [[AClass_computed_p_get_USR:.*]] | Def,Dyn,RelChild,RelAcc | rel: 1
  // CHECK-NEXT:   RelChild,RelAcc | instance-property/Swift | computed_p | [[AClass_computed_p_USR]]
  // CHECK-NOT: acc-set/Swift | setter:computed_p |
  init(x: Int) {
    y = AStruct(x: x)
    // CHECK: [[@LINE-1]]:17 | instance-property/Swift | x | [[AStruct_x_USR]] | Ref,RelCont | rel: 1
    // CHECK: [[@LINE-2]]:9 | struct/Swift | AStruct | [[AStruct_USR]] | Ref,RelCont | rel: 1

    self.z = [1, 2, 3]
  }
  subscript(index: Int) -> Int {
  // CHECK: [[@LINE-1]]:3 | instance-property/subscript/Swift | subscript(_:) | [[AClass_subscript_USR:.*]] | Def,RelChild | rel: 1
    get { return z[0] }
    // CHECK: [[@LINE-1]]:5 | instance-method/acc-get/Swift | getter:subscript(_:) | [[AClass_subscript_get_USR:.*]] | Def,Dyn,RelChild,RelAcc | rel: 1
    set { z[0] = newValue }
    // CHECK: [[@LINE-1]]:5 | instance-method/acc-set/Swift | setter:subscript(_:) | [[AClass_subscript_set_USR:.*]] | Def,Dyn,RelChild,RelAcc | rel: 1
  }
  func foo() -> Int { return z[0] }
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | foo() | [[AClass_foo_USR:.*]] | Def,Dyn,RelChild | rel: 1
}

let _ = AClass.foo
// CHECK: [[@LINE-1]]:16 | instance-method/Swift | foo() | [[AClass_foo_USR]] | Ref | rel: 0
let _ = AClass(x: 1).foo
// CHECK: [[@LINE-1]]:22 | instance-method/Swift | foo() | [[AClass_foo_USR]] | Ref | rel: 0
let _ = AClass(x: 1)[1]
// CHECK: [[@LINE-1]]:21 | instance-property/subscript/Swift | subscript(_:) | [[AClass_subscript_USR]] | Ref,Read,Dyn,RelRec | rel: 1
// CHECK-NEXT: RelRec | class/Swift | AClass | [[AClass_USR]]
// CHECK: [[@LINE-3]]:21 | instance-method/acc-get/Swift | getter:subscript(_:) | [[AClass_subscript_get_USR]] | Ref,Call,Dyn,Impl,RelRec | rel: 1
// CHECK-NEXT: RelRec | class/Swift | AClass | [[AClass_USR]]
let _ = AClass(x: 1)[1] = 2
// CHECK: [[@LINE-1]]:21 | instance-property/subscript/Swift | subscript(_:) | [[AClass_subscript_USR]] | Ref,Writ,Dyn,RelRec | rel: 1
// CHECK-NEXT: RelRec | class/Swift | AClass | [[AClass_USR]]
// CHECK: [[@LINE-3]]:21 | instance-method/acc-set/Swift | setter:subscript(_:) | [[AClass_subscript_set_USR]] | Ref,Call,Dyn,Impl,RelRec | rel: 1
// CHECK-NEXT: RelRec | class/Swift | AClass | [[AClass_USR]]

extension AClass {
  func test_property_refs1() -> AStruct {
    // CHECK: [[@LINE-1]]:8 | instance-method/Swift | test_property_refs1() | [[test_property_refs1_USR:.*]] | Def,Dyn,RelChild | rel: 1
    _ = y
    // CHECK: [[@LINE-1]]:9 | instance-property/Swift | y | [[AClass_y_USR]] | Ref,Read,Dyn,RelRec,RelCont | rel: 2
    // CHECK-NEXT: RelCont | instance-method/Swift | test_property_refs1() | [[test_property_refs1_USR]]
    // CHECK-NEXT: RelRec | class/Swift | AClass | [[AClass_USR]]
    // CHECK: [[@LINE-4]]:9 | instance-method/acc-get/Swift | getter:y | [[AClass_y_get_USR]] | Ref,Call,Dyn,Impl,RelRec,RelCall,RelCont | rel: 2
    // CHECK-NEXT: RelCall,RelCont | instance-method/Swift | test_property_refs1() | [[test_property_refs1_USR]]
    // CHECK-NEXT: RelRec | class/Swift | AClass | [[AClass_USR]]

    return y
    // CHECK: [[@LINE-1]]:12 | instance-property/Swift | y | [[AClass_y_USR]] | Ref,Read,Dyn,RelRec,RelCont | rel: 2
    // CHECK-NEXT: RelCont | instance-method/Swift | test_property_refs1() | [[test_property_refs1_USR]]
    // CHECK-NEXT: RelRec | class/Swift | AClass | [[AClass_USR]]
    // CHECK: [[@LINE-4]]:12 | instance-method/acc-get/Swift | getter:y | [[AClass_y_get_USR]] | Ref,Call,Dyn,Impl,RelRec,RelCall,RelCont | rel: 2
    // CHECK-NEXT: RelCall,RelCont | instance-method/Swift | test_property_refs1() | [[test_property_refs1_USR]]
    // CHECK-NEXT: RelRec | class/Swift | AClass | [[AClass_USR]]
  }

  func test_property_refs2() -> Int {
    // CHECK: [[@LINE-1]]:8 | instance-method/Swift | test_property_refs2() | [[test_property_refs2_USR:.*]] | Def,Dyn,RelChild | rel: 1
    _ = computed_p
    // CHECK: [[@LINE-1]]:9 | instance-property/Swift | computed_p | [[AClass_computed_p_USR]] | Ref,Read,Dyn,RelRec,RelCont | rel: 2
    // CHECK-NEXT: RelCont | instance-method/Swift | test_property_refs2() | [[test_property_refs2_USR]]
    // CHECK-NEXT: RelRec | class/Swift | AClass | [[AClass_USR]]
    // CHECK: [[@LINE-4]]:9 | instance-method/acc-get/Swift | getter:computed_p | [[AClass_computed_p_get_USR]] | Ref,Call,Dyn,Impl,RelRec,RelCall,RelCont | rel: 2
    // CHECK-NEXT: RelCall,RelCont | instance-method/Swift | test_property_refs2() | [[test_property_refs2_USR]]
    // CHECK-NEXT: RelRec | class/Swift | AClass | [[AClass_USR]]

    return computed_p
    // CHECK: [[@LINE-1]]:12 | instance-property/Swift | computed_p | [[AClass_computed_p_USR]] | Ref,Read,Dyn,RelRec,RelCont | rel: 2
    // CHECK-NEXT: RelCont | instance-method/Swift | test_property_refs2() | [[test_property_refs2_USR]]
    // CHECK-NEXT: RelRec | class/Swift | AClass | [[AClass_USR]]
    // CHECK: [[@LINE-4]]:12 | instance-method/acc-get/Swift | getter:computed_p | [[AClass_computed_p_get_USR]] | Ref,Call,Dyn,Impl,RelRec,RelCall,RelCont | rel: 2
    // CHECK-NEXT: RelCall,RelCont | instance-method/Swift | test_property_refs2() | [[test_property_refs2_USR]]
    // CHECK-NEXT: RelRec | class/Swift | AClass | [[AClass_USR]]
  }
}

// RelationBaseOf, RelationOverrideOf

protocol X {
// CHECK: [[@LINE-1]]:10 | protocol/Swift | X | [[X_USR:.*]] | Def | rel: 0

  var reqProp: Int { get }
  // CHECK: [[@LINE-1]]:7 | instance-property/Swift | reqProp | [[reqProp_USR:.*]] | Def,RelChild | rel: 1
  // CHECK: [[@LINE-2]]:22 | instance-method/acc-get/Swift | getter:reqProp | {{.*}} | Def,Dyn,RelChild,RelAcc | rel: 1
  // CHECK-NEXT: RelChild,RelAcc | instance-property/Swift | reqProp | [[reqProp_USR]]
}

protocol Y {}
// CHECK: [[@LINE-1]]:10 | protocol/Swift | Y | [[Y_USR:.*]] | Def | rel: 0

class ImplementsX : X, Y {
// CHECK: [[@LINE-1]]:7 | class/Swift | ImplementsX | [[ImplementsX_USR:.*]] | Def | rel: 0
// CHECK: [[@LINE-2]]:21 | protocol/Swift | X | [[X_USR]] | Ref,RelBase | rel: 1
// CHECK-NEXT: RelBase | class/Swift | ImplementsX | [[ImplementsX_USR]]

  var reqProp: Int { return 1 }
  // CHECK: [[@LINE-1]]:7 | instance-property/Swift | reqProp | [[reqPropImpl_USR:.*]] | Def,RelChild,RelOver | rel: 2
  // CHECK-NEXT: RelOver | instance-property/Swift | reqProp | [[reqProp_USR]]
  // CHECK-NEXT: RelChild | class/Swift | ImplementsX | [[ImplementsX_USR]]
}

func TestX(x: X) {
  _ = x.reqProp
  // CHECK: [[@LINE-1]]:9 | instance-property/Swift | reqProp | [[reqProp_USR]] | Ref,Read,Dyn,RelRec,RelCont | rel: 2
}

protocol AProtocol {
  // CHECK: [[@LINE-1]]:10 | protocol/Swift | AProtocol | [[AProtocol_USR:.*]] | Def | rel: 0

  associatedtype T : X where T:Y
  // CHECK: [[@LINE-1]]:18 | type-alias/associated-type/Swift | T | [[AProtocol_T_USR:.*]] | Def,RelChild | rel: 1
  // CHECK-NEXT: RelChild | protocol/Swift | AProtocol | [[AProtocol_USR]]
  // CHECK: [[@LINE-3]]:22 | protocol/Swift | X | [[X_USR]] | Ref | rel: 0
  // CHECK: [[@LINE-4]]:30 | type-alias/associated-type/Swift | T | [[AProtocol_T_USR]] | Ref | rel: 0
  // CHECK: [[@LINE-5]]:32 | protocol/Swift | Y | [[Y_USR]] | Ref | rel: 0

  func foo() -> Int
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | foo() | s:14swift_ide_test9AProtocolP3fooSiyF | Def,Dyn,RelChild | rel: 1
  // CHECK-NEXT: RelChild | protocol/Swift | AProtocol | s:14swift_ide_test9AProtocolP
}

protocol QBase {
  associatedtype A
}

protocol Q : QBase where Self.A: AProtocol {}
// CHECK: [[@LINE-1]]:34 | protocol/Swift | AProtocol | [[AProtocol_USR]] | Ref | rel: 0

class ASubClass : AClass, AProtocol {
// CHECK: [[@LINE-1]]:7 | class/Swift | ASubClass | s:14swift_ide_test9ASubClassC | Def | rel: 0
// CHECK: [[@LINE-2]]:19 | class/Swift | AClass | s:14swift_ide_test6AClassC | Ref,RelBase | rel: 1
// CHECK-NEXT: RelBase | class/Swift | ASubClass | s:14swift_ide_test9ASubClassC
// CHECK: [[@LINE-4]]:27 | protocol/Swift | AProtocol | s:14swift_ide_test9AProtocolP | Ref,RelBase | rel: 1
// CHECK-NEXT: RelBase | class/Swift | ASubClass | s:14swift_ide_test9ASubClassC

  typealias T = ImplementsX

  override func foo() -> Int {
    // CHECK: [[@LINE-1]]:17 | instance-method/Swift | foo() | s:14swift_ide_test9ASubClassC3fooSiyF | Def,Dyn,RelChild,RelOver | rel: 3
    // CHECK-NEXT: RelOver | instance-method/Swift | foo() | [[AClass_foo_USR]]
    // CHECK-NEXT: RelOver | instance-method/Swift | foo() | s:14swift_ide_test9AProtocolP3fooSiyF
    // CHECK-NEXT: RelChild | class/Swift | ASubClass | s:14swift_ide_test9ASubClassC
    return 1
  }
}

// RelationExtendedBy
extension AClass {
  // CHECK: [[@LINE-1]]:11 | extension/ext-class/Swift | AClass | [[EXT_ACLASS_USR:.*]] | Def | rel: 0
  // CHECK: [[@LINE-2]]:11 | class/Swift | AClass | s:14swift_ide_test6AClassC | Ref,RelExt | rel: 1
  // CHECK-NEXT: RelExt | extension/ext-class/Swift | AClass | [[EXT_ACLASS_USR]]

  func bar() -> Int { return 2 }
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | bar() | s:14swift_ide_test6AClassC3barSiyF | Def,Dyn,RelChild | rel: 1
  // CHECK-NEXT: RelChild | extension/ext-class/Swift | AClass | [[EXT_ACLASS_USR]]
}

struct OuterS {
// CHECK: [[@LINE-1]]:8 | struct/Swift | OuterS | [[OUTERS_USR:.*]] | Def | rel: 0
  struct InnerS {}
  // CHECK: [[@LINE-1]]:10 | struct/Swift | InnerS | [[INNERS_USR:.*]] | Def,RelChild | rel: 1
  // CHECK-NEXT: RelChild | struct/Swift | OuterS | [[OUTERS_USR]]
}
extension OuterS.InnerS : AProtocol {
  // CHECK: [[@LINE-1]]:18 | extension/ext-struct/Swift | InnerS | [[EXT_INNERS_USR:.*]] | Def | rel: 0
  // CHECK: [[@LINE-2]]:18 | struct/Swift | InnerS | [[INNERS_USR]] | Ref,RelExt | rel: 1
  // CHECK-NEXT: RelExt | extension/ext-struct/Swift | InnerS | [[EXT_INNERS_USR]]
  // CHECK: [[@LINE-4]]:27 | protocol/Swift | AProtocol | [[AProtocol_USR]] | Ref,RelBase | rel: 1
  // CHECK-NEXT: RelBase | extension/ext-struct/Swift | InnerS | [[EXT_INNERS_USR]]
  // CHECK: [[@LINE-6]]:11 | struct/Swift | OuterS | [[OUTERS_USR]] | Ref | rel: 0

  typealias T = ImplementsX
  func foo() -> Int { return 1 }
}

protocol ExtendMe {}
protocol Whatever {}
// CHECK: [[@LINE-1]]:10 | protocol/Swift | Whatever | [[Whatever_USR:.*]] | Def | rel: 0
extension ExtendMe where Self: Whatever {}
// CHECK: [[@LINE-1]]:32 | protocol/Swift | Whatever | [[Whatever_USR]] | Ref | rel: 0

var anInstance = AClass(x: 1)
// CHECK: [[@LINE-1]]:18 | class/Swift | AClass | s:14swift_ide_test6AClassC | Ref,RelCont | rel: 1
// CHECK: [[@LINE-2]]:18 | constructor/Swift | init(x:) | s:14swift_ide_test6AClassC1xACSi_tcfc | Ref,Call,RelCont | rel: 1

anInstance.y.x = anInstance.y.x
// CHECK: [[@LINE-1]]:1 | variable/Swift | anInstance | s:14swift_ide_test10anInstanceAA6AClassCvp | Ref,Read | rel: 0
// CHECK: [[@LINE-2]]:12 | instance-property/Swift | y | [[AClass_y_USR]] | Ref,Read,Writ,Dyn,RelRec | rel: 1
// CHECK: [[@LINE-3]]:14 | instance-property/Swift | x | [[AStruct_x_USR]] | Ref,Writ | rel: 0
// CHECK: [[@LINE-4]]:18 | variable/Swift | anInstance | s:14swift_ide_test10anInstanceAA6AClassCvp | Ref,Read | rel: 0
// CHECK: [[@LINE-5]]:29 | instance-property/Swift | y | [[AClass_y_USR]] | Ref,Read,Dyn,RelRec | rel: 1
// CHECK: [[@LINE-6]]:31 | instance-property/Swift | x | [[AStruct_x_USR]] | Ref,Read | rel: 0

anInstance.y.aMethod()
// CHECK: [[@LINE-1]]:1 | variable/Swift | anInstance | s:14swift_ide_test10anInstanceAA6AClassCvp | Ref,Read | rel: 0
// CHECK: [[@LINE-2]]:12 | instance-property/Swift | y | [[AClass_y_USR]] | Ref,Read,Writ,Dyn,RelRec | rel: 1
// CHECK: [[@LINE-3]]:14 | instance-method/Swift | aMethod() | s:14swift_ide_test7AStructV7aMethodyyF | Ref,Call | rel: 0

// FIXME Write role of z occurrence on the RHS?
anInstance.z[1] = anInstance.z[0]
// CHECK: [[@LINE-1]]:1 | variable/Swift | anInstance | s:14swift_ide_test10anInstanceAA6AClassCvp | Ref,Read | rel: 0
// CHECK: [[@LINE-2]]:12 | instance-property/Swift | z | s:14swift_ide_test6AClassC1zSaySiGvp | Ref,Read,Writ,Dyn,RelRec | rel: 1
// CHECK: [[@LINE-3]]:19 | variable/Swift | anInstance | s:14swift_ide_test10anInstanceAA6AClassCvp | Ref,Read | rel: 0
// CHECK: [[@LINE-4]]:30 | instance-property/Swift | z | s:14swift_ide_test6AClassC1zSaySiGvp | Ref,Read,Writ,Dyn,RelRec | rel: 1

let otherInstance = AStruct(x: 1)
// CHECK: [[@LINE-1]]:29 | instance-property/Swift | x | [[AStruct_x_USR]] | Ref,RelCont | rel: 1
// CHECK: [[@LINE-2]]:21 | struct/Swift | AStruct | [[AStruct_USR]] | Ref,RelCont | rel: 1

_ = AStruct.init(x:)
// CHECK: [[@LINE-1]]:18 | instance-property/Swift | x | [[AStruct_x_USR]] | Ref | rel: 0
// CHECK: [[@LINE-2]]:5 | struct/Swift | AStruct | [[AStruct_USR]] | Ref | rel: 0

let _ = otherInstance[0]
// CHECK: [[@LINE-1]]:9 | variable/Swift | otherInstance | s:14swift_ide_test13otherInstanceAA7AStructVvp | Ref,Read | rel: 0
// CHECK: [[@LINE-2]]:22 | instance-property/subscript/Swift | subscript(_:) | s:14swift_ide_test7AStructVyS2icip | Ref,Read | rel: 0

let _ = anInstance[0]
// CHECK: [[@LINE-1]]:9 | variable/Swift | anInstance | s:14swift_ide_test10anInstanceAA6AClassCvp | Ref,Read | rel: 0
// CHECK: [[@LINE-2]]:19 | instance-property/subscript/Swift | subscript(_:) | [[AClass_subscript_USR]] | Ref,Read,Dyn,RelRec | rel: 1

let aSubInstance: AClass = ASubClass(x: 1)
// CHECK: [[@LINE-1]]:5 | variable/Swift | aSubInstance | s:14swift_ide_test12aSubInstanceAA6AClassCvp | Def | rel: 0
// CHECK: [[@LINE-2]]:28 | class/Swift | ASubClass | s:14swift_ide_test9ASubClassC | Ref,RelCont | rel: 1

// Dynamic, RelationReceivedBy
let _ = aSubInstance.foo()
// CHECK: [[@LINE-1]]:9 | variable/Swift | aSubInstance | s:14swift_ide_test12aSubInstanceAA6AClassCvp | Ref,Read | rel: 0
// CHECK: [[@LINE-2]]:22 | instance-method/Swift | foo() | [[AClass_foo_USR]] | Ref,Call,Dyn,RelRec | rel: 1
// CHECK-NEXT: RelRec | class/Swift | AClass | s:14swift_ide_test6AClassC

// RelationContainedBy
let contained = 2
// CHECK: [[@LINE-1]]:5 | variable/Swift | contained | s:14swift_ide_test9containedSivp | Def | rel: 0

protocol ProtRoot {
  func fooCommon()
  func foo1()
  func foo2()
  func foo3(a : Int)
  func foo3(a : String)
}

protocol ProtDerived : ProtRoot {
  func fooCommon()
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | fooCommon() | s:14swift_ide_test11ProtDerivedP9fooCommonyyF | Def,Dyn,RelChild,RelOver | rel: 2

  func bar1()
  func bar2()
  func bar3(_ : Int)
  func bar3(_ : String)
}

extension ProtDerived {
  func fooCommon() {}
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | fooCommon() | s:14swift_ide_test11ProtDerivedPAAE9fooCommonyyF | Def,Dyn,RelChild,RelOver | rel: 2
  // CHECK-NEXT: RelOver | instance-method/Swift | fooCommon() | s:14swift_ide_test11ProtDerivedP9fooCommonyyF

  func foo1() {}
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | foo1() | s:14swift_ide_test11ProtDerivedPAAE4foo1yyF | Def,Dyn,RelChild,RelOver | rel: 2
  // CHECK-NEXT: RelOver | instance-method/Swift | foo1() | s:14swift_ide_test8ProtRootP4foo1yyF

  func bar1() {}
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | bar1() | s:14swift_ide_test11ProtDerivedPAAE4bar1yyF | Def,Dyn,RelChild,RelOver | rel: 2
  // CHECK-NEXT: RelOver | instance-method/Swift | bar1() | s:14swift_ide_test11ProtDerivedP4bar1yyF

  func foo3(a : Int) {}
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | foo3(a:) | s:14swift_ide_test11ProtDerivedPAAE4foo31aySi_tF | Def,Dyn,RelChild,RelOver | rel: 2
  // CHECK-NEXT: RelOver | instance-method/Swift | foo3(a:) | s:14swift_ide_test8ProtRootP4foo31aySi_tF

  func foo3(a : String) {}
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | foo3(a:) | s:14swift_ide_test11ProtDerivedPAAE4foo31aySS_tF | Def,Dyn,RelChild,RelOver | rel: 2
  // CHECK-NEXT: RelOver | instance-method/Swift | foo3(a:) | s:14swift_ide_test8ProtRootP4foo31aySS_tF

  func bar3(_ : Int) {}
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | bar3(_:) | s:14swift_ide_test11ProtDerivedPAAE4bar3yySiF | Def,Dyn,RelChild,RelOver | rel: 2
  // CHECK-NEXT: RelOver | instance-method/Swift | bar3(_:) | s:14swift_ide_test11ProtDerivedP4bar3yySiF
}

enum MyEnum {
  init() {}
  func enum_func() {}
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | enum_func() | [[MyEnum_enum_func_USR:.*]] | Def,RelChild | rel: 1
}

MyEnum().enum_func()
// CHECK: [[@LINE-1]]:10 | instance-method/Swift | enum_func() | [[MyEnum_enum_func_USR]] | Ref,Call | rel: 0

class ClassWithFinals {
  final var prop : Int { get { return 0} }
  // CHECK: [[@LINE-1]]:26 | instance-method/acc-get/Swift | {{.*}} | Def,RelChild,RelAcc | rel: 1
  final var prop2 = 0
  // CHECK: [[@LINE-1]]:13 | instance-method/acc-get/Swift | {{.*}} | Def,Impl,RelChild,RelAcc | rel: 1
  // CHECK: [[@LINE-2]]:13 | instance-method/acc-set/Swift | {{.*}} | Def,Impl,RelChild,RelAcc | rel: 1
  final func foo() {}
  // CHECK: [[@LINE-1]]:14 | instance-method/Swift | {{.*}} | Def,RelChild | rel: 1
}
final class FinalClass {
  var prop : Int { get { return 0} }
  // CHECK: [[@LINE-1]]:20 | instance-method/acc-get/Swift | {{.*}} | Def,RelChild,RelAcc | rel: 1
  var prop2 = 0
  // CHECK: [[@LINE-1]]:7 | instance-method/acc-get/Swift | {{.*}} | Def,Impl,RelChild,RelAcc | rel: 1
  // CHECK: [[@LINE-2]]:7 | instance-method/acc-set/Swift | {{.*}} | Def,Impl,RelChild,RelAcc | rel: 1
  func foo() {}
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | {{.*}} | Def,RelChild | rel: 1
}

struct StructWithKeypath {
  var x: Int = 0

  subscript(idx: Int) -> Int { get { } set { } }
}

_ = \StructWithKeypath.x
// CHECK: [[@LINE-1]]:24 | instance-property/Swift | x | s:14swift_ide_test17StructWithKeypathV1xSivp | Ref,Read | rel: 0
// CHECK: [[@LINE-2]]:24 | instance-method/acc-get/Swift | getter:x | s:14swift_ide_test17StructWithKeypathV1xSivg | Ref,Call,Impl | rel: 0

_ = \StructWithKeypath.[0]
// CHECK: [[@LINE-1]]:24 | instance-property/subscript/Swift | subscript(_:) | s:14swift_ide_test17StructWithKeypathVyS2icip | Ref,Read | rel: 0
// CHECK: [[@LINE-2]]:24 | instance-method/acc-get/Swift | getter:subscript(_:) | s:14swift_ide_test17StructWithKeypathVyS2icig | Ref,Call,Impl | rel: 0

internal protocol FromInt {
    init(_ uint64: Int)
}
extension Int: FromInt { }
func test<M>(_: M, value: Int?) {
    if let idType = M.self as? FromInt.Type {
        _ = value.flatMap(idType.init) as? M
// CHECK: [[@LINE-1]]:34 | constructor/Swift | init(_:) | s:14swift_ide_test7FromIntPyxSicfc | Ref,RelCont | rel: 1
    }
}

func `escapedName`(`x`: Int) {}
// CHECK: [[@LINE-1]]:6 | function/Swift | escapedName(x:) | {{.*}} | Def | rel: 0
`escapedName`(`x`: 2)
// CHECK: [[@LINE-1]]:1 | function/Swift | escapedName(x:) | {{.*}} | Ref,Call | rel: 0
`escapedName`(`x`:)
// CHECK: [[@LINE-1]]:1 | function/Swift | escapedName(x:) | {{.*}} | Ref | rel: 0

protocol WithPrimary<Assoc> {
// CHECK: [[@LINE-1]]:22 | type-alias/associated-type/Swift | Assoc | {{.*}} | Ref | rel: 0
// CHECK: [[@LINE-2]]:10 | protocol/Swift | WithPrimary | {{.*}} | Def | rel: 0
  associatedtype Assoc
// CHECK: [[@LINE-1]]:18 | type-alias/associated-type/Swift | Assoc | {{.*}} | Def,RelChild | rel: 1
// CHECK-NEXT: RelChild | protocol/Swift | WithPrimary | {{.*}}
}

struct Root {} // CHECK: [[@LINE]]:8 | struct/Swift | Root | [[Root_USR:.*]] | Def | rel: 0

typealias Alias = Root // CHECK: [[@LINE]]:11 | type-alias/Swift | Alias | [[Alias_USR:.*]] | Def | rel: 0

extension Alias {
  // CHECK: [[@LINE-1]]:11 | type-alias/Swift | Alias | [[Alias_USR]] | Ref | rel: 0
  // CHECK: [[@LINE-2]]:11 | struct/Swift | Root | [[Root_USR]] | Ref,Impl,RelExt | rel: 1
  func empty() {}
}
