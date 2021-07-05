// RUN: %target-swift-ide-test -print-indexed-symbols -source-filename %s | %FileCheck %s

// Enum
enum AnEnumeration {
// CHECK: [[@LINE-1]]:6 | enum/Swift | AnEnumeration | s:14swift_ide_test13AnEnumerationO | Def | rel: 0

  // EnumElement
  case Element
  // CHECK: [[@LINE-1]]:8 | enumerator/Swift | Element | s:14swift_ide_test13AnEnumerationO7ElementyA2CmF | Def,RelChild | rel: 1
  // CHECK-NEXT: RelChild | enum/Swift | AnEnumeration | s:14swift_ide_test13AnEnumerationO
}

// Struct
struct AStruct {
  // CHECK: [[@LINE-1]]:8 | struct/Swift | AStruct | s:14swift_ide_test7AStructV | Def | rel: 0
  
  var base: UnsafeMutablePointer<Int>

  // Subscript
  subscript(index: Int) -> Int {
    // CHECK: [[@LINE-1]]:3 | instance-property/subscript/Swift | subscript(_:) | s:14swift_ide_test7AStructVyS2icip | Def,RelChild | rel: 1
    // CHECK-NEXT: RelChild | struct/Swift | AStruct | s:14swift_ide_test7AStructV

    // Accessor + AccessorAddressor
    unsafeAddress {
      // CHECK: [[@LINE-1]]:5 | instance-method/acc-addr/Swift |  | s:14swift_ide_test7AStructVyS2icilu | Def,RelChild,RelAcc | rel: 1
      // CHECK-NEXT: RelChild,RelAcc | instance-property/subscript/Swift | subscript(_:) | s:14swift_ide_test7AStructVyS2icip

      return UnsafePointer(base)
    }

    // Accessor + AccessorMutableAddressor
    unsafeMutableAddress {
      // CHECK: [[@LINE-1]]:5 | instance-method/acc-mutaddr/Swift |  | s:14swift_ide_test7AStructVyS2iciau | Def,RelChild,RelAcc | rel: 1
      // CHECK-NEXT: RelChild,RelAcc | instance-property/subscript/Swift | subscript(_:) | s:14swift_ide_test7AStructVyS2icip

      return base
    }
  }
  // CHECK: [[@LINE-20]]:13 | param/Swift | index | {{.*}} | Def,RelChild | rel: 1
  // CHECK: [[@LINE-21]]:20 | struct/Swift | Int | {{.*}} | Ref | rel: 0
  // CHECK: [[@LINE-22]]:28 | struct/Swift | Int | {{.*}} | Ref | rel: 0
}

// Class
class AClass {
  // CHECK: [[@LINE-1]]:7 | class/Swift | AClass | s:14swift_ide_test6AClassC | Def | rel: 0

  // InstanceMethod + Parameters
  func instanceMethod(a: Int, b b: Int, _ c: Int, d _: Int, _: Int) {
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | instanceMethod(a:b:_:d:_:) | s:14swift_ide_test6AClassC14instanceMethod1a1b_1d_ySi_S4itF | Def,Dyn,RelChild | rel: 1
  // CHECK-NEXT: RelChild | class/Swift | AClass | s:14swift_ide_test6AClassC
  // CHECK: [[@LINE-3]]:23 | param/Swift | a | s:14swift_ide_test6AClassC14instanceMethod1a1b_1d_ySi_S4itFAEL_Sivp | Def,RelChild | rel: 1
  // CHECK-NEXT: RelChild | instance-method/Swift | instanceMethod(a:b:_:d:_:) | s:14swift_ide_test6AClassC14instanceMethod1a1b_1d_ySi_S4itF
  // CHECK: [[@LINE-5]]:33 | param(local)/Swift | b | s:{{.*}} | Def,RelChild | rel: 1
  // CHECK: [[@LINE-6]]:43 | param(local)/Swift | c | s:{{.*}} | Def,RelChild | rel: 1
  // CHECK: [[@LINE-7]]:53 | param(local)/Swift | _ | s:{{.*}} | Def,RelChild | rel: 1
  // CHECK: [[@LINE-8]]:61 | param/Swift | _ | s:{{.*}} | Def,RelChild | rel: 1

    _ = a
    // CHECK: [[@LINE-1]]:9 | param/Swift | a | s:{{.*}} | Ref,Read,RelCont | rel: 1
    _ = b
    // CHECK-NOT: [[@LINE-1]]:9 | param(local)/Swift | b | s:{{.*}} | Ref,Read,RelCont | rel: 1
    _ = c
    // CHECK-NOT: [[@LINE-1]]:9 | param(local)/Swift | c | s:{{.*}} | Ref,Read,RelCont | rel: 1
  }

  // ClassMethod
  class func classMethod() {}
  // CHECK: [[@LINE-1]]:14 | class-method/Swift | classMethod() | s:14swift_ide_test6AClassC11classMethodyyFZ | Def,Dyn,RelChild | rel: 1
  // CHECK-NEXT: RelChild | class/Swift | AClass | s:14swift_ide_test6AClassC

  // StaticMethod
  static func staticMethod() {}
  // CHECK: [[@LINE-1]]:15 | static-method/Swift | staticMethod() | s:14swift_ide_test6AClassC12staticMethodyyFZ | Def,RelChild | rel: 1
  // CHECK-NEXT: RelChild | class/Swift | AClass | s:14swift_ide_test6AClassC

  // InstanceProperty
  var instanceProperty: Int {
    // CHECK: [[@LINE-1]]:7 | instance-property/Swift | instanceProperty | s:14swift_ide_test6AClassC16instancePropertySivp | Def,RelChild | rel: 1
    // CHECK-NEXT: RelChild | class/Swift | AClass | s:14swift_ide_test6AClassC

    // Accessor + AccessorGetter
    get {
      // CHECK: [[@LINE-1]]:5 | instance-method/acc-get/Swift | getter:instanceProperty | s:14swift_ide_test6AClassC16instancePropertySivg | Def,Dyn,RelChild,RelAcc | rel: 1
      // CHECK-NEXT: RelChild,RelAcc | instance-property/Swift | instanceProperty | s:14swift_ide_test6AClassC16instancePropertySiv

      return 1
    }

    // Accessor + AccessorSetter
    set {}
    // CHECK: [[@LINE-1]]:5 | instance-method/acc-set/Swift | setter:instanceProperty | s:14swift_ide_test6AClassC16instancePropertySivs | Def,Dyn,RelChild,RelAcc | rel: 1
    // CHECK-NEXT: RelChild,RelAcc | instance-property/Swift | instanceProperty | s:14swift_ide_test6AClassC16instancePropertySiv
  }

  var observed = 0 {

    // Accessor + AccessorWillSet
    willSet {}
    // CHECK: [[@LINE-1]]:5 | instance-method/acc-willset/Swift | willSet:observed | s:14swift_ide_test6AClassC8observedSivw | Def,RelChild,RelAcc | rel: 1
    // CHECK-NEXT: RelChild,RelAcc | instance-property/Swift | observed | s:14swift_ide_test6AClassC8observedSiv

    // Accessor + AccessorDidSet
    didSet {}
    // CHECK: [[@LINE-1]]:5 | instance-method/acc-didset/Swift | didSet:observed | s:14swift_ide_test6AClassC8observedSivW | Def,RelChild,RelAcc | rel: 1
    // CHECK-NEXT: RelChild,RelAcc | instance-property/Swift | observed | s:14swift_ide_test6AClassC8observedSiv
  }

  // ClassProperty
  class var classProperty: Int!
  // CHECK: [[@LINE-1]]:13 | class-property/Swift | classProperty | s:14swift_ide_test6AClassC13classPropertySiSgvpZ | Def,RelChild | rel: 1
  // CHECK-NEXT: RelChild | class/Swift | AClass | s:14swift_ide_test6AClassC
  // CHECK: [[@LINE-3]]:13 | class-method/acc-get/Swift | getter:classProperty | s:14swift_ide_test6AClassC13classPropertySiSgvgZ | Def,Dyn,Impl,RelChild,RelAcc | rel: 1

  // StaticProperty
  static var staticProperty: Int!
  // CHECK: [[@LINE-1]]:14 | static-property/Swift | staticProperty | s:14swift_ide_test6AClassC14staticPropertySiSgvpZ | Def,RelChild | rel: 1
  // CHECK-NEXT: RelChild | class/Swift | AClass | s:14swift_ide_test6AClassC
  // CHECK: [[@LINE-3]]:14 | static-method/acc-get/Swift | getter:staticProperty | s:14swift_ide_test6AClassC14staticPropertySiSgvgZ | Def,Impl,RelChild,RelAcc | rel: 1

  // Constructor
  init() {}
  // CHECK: [[@LINE-1]]:3 | constructor/Swift | init() | s:14swift_ide_test6AClassCACycfc | Def,RelChild | rel: 1
  // CHECK-NEXT: RelChild | class/Swift | AClass | s:14swift_ide_test6AClassC

  // Destructor
  deinit {}
  // CHECK: [[@LINE-1]]:3 | destructor/Swift | deinit | s:14swift_ide_test6AClassCfd | Def,RelChild | rel: 1
  // CHECK-NEXT: RelChild | class/Swift | AClass | s:14swift_ide_test6AClassC
}

// Protocol
protocol AProtocol {
  // CHECK: [[@LINE-1]]:10 | protocol/Swift | AProtocol | s:14swift_ide_test9AProtocolP | Def | rel: 0

  // AssociatedType
  associatedtype T
  // CHECK: [[@LINE-1]]:18 | type-alias/associated-type/Swift | T | s:14swift_ide_test9AProtocolP1TQa | Def,RelChild | rel: 1
  // CHECK-NEXT: RelChild | protocol/Swift | AProtocol | s:14swift_ide_test9AProtocolP
}

// Extension
extension AnEnumeration { func extFn() {} }
// CHECK: [[@LINE-1]]:11 | extension/ext-enum/Swift | AnEnumeration | [[EXT_AnEnumeration_USR:s:e:s:14swift_ide_test13AnEnumerationO5extFnyyF]] | Def | rel: 0
// CHECK: [[@LINE-2]]:11 | enum/Swift | AnEnumeration | s:14swift_ide_test13AnEnumerationO | Ref,RelExt | rel: 1
// CHECK-NEXT: RelExt | extension/ext-enum/Swift | AnEnumeration | [[EXT_AnEnumeration_USR]]

extension AStruct { func extFn() {} }
// CHECK: [[@LINE-1]]:11 | extension/ext-struct/Swift | AStruct | [[EXT_AStruct_USR:s:e:s:14swift_ide_test7AStructV5extFnyyF]] | Def | rel: 0
// CHECK: [[@LINE-2]]:11 | struct/Swift | AStruct | s:14swift_ide_test7AStructV | Ref,RelExt | rel: 1
// CHECK-NEXT: RelExt | extension/ext-struct/Swift | AStruct | [[EXT_AStruct_USR]]

extension AClass { func extFn() {} }
// CHECK: [[@LINE-1]]:11 | extension/ext-class/Swift | AClass | [[EXT_AClass_USR:s:e:s:14swift_ide_test6AClassC5extFnyyF]] | Def | rel: 0
// CHECK: [[@LINE-2]]:11 | class/Swift | AClass | s:14swift_ide_test6AClassC | Ref,RelExt | rel: 1
// CHECK-NEXT: RelExt | extension/ext-class/Swift | AClass | [[EXT_AClass_USR]]

extension AProtocol { func extFn() }
// CHECK: [[@LINE-1]]:11 | extension/ext-protocol/Swift | AProtocol | [[EXT_AProtocol_USR:s:e:s:14swift_ide_test9AProtocolPAAE5extFnyyF]] | Def | rel: 0
// CHECK: [[@LINE-2]]:11 | protocol/Swift | AProtocol | s:14swift_ide_test9AProtocolP | Ref,RelExt | rel: 1
// CHECK-NEXT: RelExt | extension/ext-protocol/Swift | AProtocol | [[EXT_AProtocol_USR]]

// TypeAlias
typealias SomeAlias = AStruct
// CHECK: [[@LINE-1]]:11 | type-alias/Swift | SomeAlias | s:14swift_ide_test9SomeAliasa | Def | rel: 0
// CHECK: [[@LINE-2]]:23 | struct/Swift | AStruct | s:14swift_ide_test7AStructV | Ref | rel: 0

// GenericTypeParam
struct GenericStruct<ATypeParam> {}
// CHECK: [[@LINE-1]]:22 | type-alias/generic-type-param/Swift | ATypeParam | s:14swift_ide_test13GenericStructV10ATypeParamxmfp | Def,RelChild | rel: 1
// CHECK-NEXT: RelChild | struct/Swift | GenericStruct | s:14swift_ide_test13GenericStructV

func GenericFunc<ATypeParam>(_: ATypeParam) {}
// CHECK-NOT: [[@LINE-1]]:18 | type-alias/generic-type-param/Swift | ATypeParam | {{.*}} | Def,RelChild | rel: 1

// Function
func EmptyFunction() {}
// CHECK: [[@LINE-1]]:6 | function/Swift | EmptyFunction() | s:14swift_ide_test13EmptyFunctionyyF | Def | rel: 0

// Variable
var foo = 1
// CHECK: [[@LINE-1]]:5 | variable/Swift | foo | s:14swift_ide_test3fooSivp | Def | rel: 0

// PrefixOperator
prefix func -(a: AStruct) -> AStruct { return a }
// CHECK: [[@LINE-1]]:13 | function/prefix-operator/Swift | -(_:) | s:14swift_ide_test1sopyAA7AStructVADF | Def | rel: 0

// PostfixOperator
postfix operator ++
postfix func ++(a: AStruct) -> AStruct { return a }
// CHECK: [[@LINE-1]]:14 | function/postfix-operator/Swift | ++(_:) | s:14swift_ide_test2ppoPyAA7AStructVADF | Def | rel: 0

// InfixOperator
func +(a: AStruct, b: AStruct) -> AStruct { return a }
// CHECK: [[@LINE-1]]:6 | function/infix-operator/Swift | +(_:_:) | s:14swift_ide_test1poiyAA7AStructVAD_ADtF | Def | rel: 0

class XCTestCase {}
class MyTestCase : XCTestCase {
// CHECK: [[@LINE-1]]:7 | class(test)/Swift | MyTestCase |
  func callit() {}
  func testMe() {
  // CHECK: [[@LINE-1]]:8 | instance-method(test)/Swift | testMe() | [[MyTestCase_testMe_USR:.*]] | Def,Dyn,RelChild
    callit()
    // CHECK: [[@LINE-1]]:5 | instance-method/Swift | callit() | s:14swift_ide_test10MyTestCaseC6callityyF | Ref,Call,Dyn,RelRec,RelCall,RelCont | rel: 2
    // CHECK-NEXT: RelCall,RelCont | instance-method(test)/Swift | testMe() | [[MyTestCase_testMe_USR]]
  }
  func testResult() -> Int? { return nil }
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | testResult() |
  func test(withInt: Int) {}
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | test(withInt:) |
}
class SubTestCase : MyTestCase {
// CHECK: [[@LINE-1]]:7 | class(test)/Swift | SubTestCase | [[SubTestCase_USR:.*]] | Def | rel: 0
  func testIt2() {}
  // CHECK: [[@LINE-1]]:8 | instance-method(test)/Swift | testIt2() |
}
extension SubTestCase {
// CHECK: [[@LINE-1]]:11 | extension/ext-class(test)/Swift | SubTestCase | [[SubTestCaseExt_USR:.*]] | Def | rel: 0
// CHECK: [[@LINE-2]]:11 | class(test)/Swift | SubTestCase | [[SubTestCase_USR]] | Ref,RelExt | rel: 1
// CHECK-NEXT: RelExt | extension/ext-class(test)/Swift | SubTestCase | [[SubTestCaseExt_USR]]
  func testIt3() {}
  // CHECK: [[@LINE-1]]:8 | instance-method(test)/Swift | testIt3() |
}
class NonTestCase {
  func testMeNot() {}
  // CHECK: [[@LINE-1]]:8 | instance-method/Swift | testMeNot() |
}

// CHECK: [[@LINE+1]]:7 | class/Swift | C1 | [[C1_USR:.*]] | Def | rel: 0
class C1 {}
// CHECK: [[@LINE+1]]:11 | type-alias/Swift | C1Alias | [[C1Alias_USR:.*]] | Def | rel: 0
typealias C1Alias = C1
// CHECK: [[@LINE+4]]:7 | class/Swift | SubC1 | [[SubC1_USR:.*]] | Def | rel: 0
// CHECK: [[@LINE+3]]:15 | type-alias/Swift | C1Alias | [[C1Alias_USR]] | Ref | rel: 0
// CHECK: [[@LINE+2]]:15 | class/Swift | C1 | [[C1_USR]] | Ref,Impl,RelBase | rel: 1
// CHECK-NEXT: RelBase | class/Swift | SubC1 | [[SubC1_USR]]
class SubC1 : C1Alias {}

struct ImplCtors {
// CHECK: [[@LINE-1]]:8 | struct/Swift | ImplCtors | [[ImplCtors_USR:.*]] | Def | rel: 0
  // CHECK: [[@LINE-2]]:8 | constructor/Swift | init(x:) | [[ImplCtors_init_with_param_USR:.*]] | Def,Impl,RelChild | rel: 1
  // CHECK-NEXT: RelChild | struct/Swift | ImplCtors | [[ImplCtors_USR]]
  // CHECK: [[@LINE-4]]:8 | constructor/Swift | init() | [[ImplCtors_init_USR:.*]] | Def,Impl,RelChild | rel: 1
  // CHECK-NEXT: RelChild | struct/Swift | ImplCtors | [[ImplCtors_USR]]
  var x = 0
}
_ = ImplCtors()
// CHECK: [[@LINE-1]]:5 | constructor/Swift | init() | [[ImplCtors_init_USR]] | Ref,Call | rel: 0
_ = ImplCtors(x:0)
// CHECK: [[@LINE-1]]:5 | constructor/Swift | init(x:) | [[ImplCtors_init_with_param_USR]] | Ref,Call | rel: 0

var globalCompProp: Int // CHECK: [[@LINE]]:5 | variable/Swift | [[globalCompProp:.*]] | Def | rel: 0
{ // CHECK: [[@LINE-1]]:5 | function/acc-get/Swift | getter:globalCompProp | [[globalCompProp:.*]] | Def,Impl,RelChild,RelAcc | rel: 1
  // CHECK-NEXT: RelChild,RelAcc | variable/Swift | [[globalCompProp:.*]]
  // Check that the accessor def is not showing up twice.
  // CHECK-NOT: [[@LINE-3]]:1 | function/acc-get/Swift
  return 0
}
