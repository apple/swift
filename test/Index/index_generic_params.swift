// RUN: %target-swift-ide-test -print-indexed-symbols -source-filename %s | %FileCheck %s

// CHECK: [[@LINE+1]]:10 | protocol/Swift | P1 | s:14swift_ide_test2P1P | Def |
protocol P1 {
  // CHECK: [[@LINE+1]]:18 | type-alias/associated-type/Swift | Assoc | s:14swift_ide_test2P1P5AssocQa | Def,RelChild |
  associatedtype Assoc
}

// CHECK: [[@LINE+1]]:10 | protocol/Swift | P2 | s:14swift_ide_test2P2P | Def |
protocol P2 {}

// MARK: - Test extening a simple generic type

// CHECK: [[@LINE+4]]:7 | class/Swift | Foo | s:14swift_ide_test3FooC | Def |
// CHECK: [[@LINE+3]]:11 | type-alias/generic-type-param/Swift | OtherParam | s:14swift_ide_test3FooC10OtherParamxmfp | Def,RelChild |
// CHECK: [[@LINE+2]]:23 | type-alias/generic-type-param/Swift | Bar | s:14swift_ide_test3FooC3Barq_mfp | Def,RelChild |
// CHECK: [[@LINE+1]]:28 | protocol/Swift | P1 | s:14swift_ide_test2P1P | Ref |
class Foo<OtherParam, Bar: P1> {}

// CHECK: [[@LINE+4]]:11 | extension/ext-class/Swift | Foo | s:e:s:14swift_ide_test3FooCA2A2P2R_rlE3foo1xyq__tF | Def |
// CHECK: [[@LINE+3]]:11 | class/Swift | Foo | s:14swift_ide_test3FooC | Ref,RelExt |
// CHECK: [[@LINE+2]]:21 | type-alias/generic-type-param/Swift | Bar | s:14swift_ide_test3FooC3Barq_mfp | Ref |
// CHECK: [[@LINE+1]]:26 | protocol/Swift | P2 | s:14swift_ide_test2P2P | Ref |
extension Foo where Bar: P2 {
// CHECK: [[@LINE+1]]:15 | type-alias/generic-type-param/Swift | Bar | s:14swift_ide_test3FooC3Barq_mfp | Ref,RelCont |
  func foo(x: Bar) {}

// CHECK: [[@LINE+2]]:15 | type-alias/generic-type-param/Swift | Bar | s:14swift_ide_test3FooC3Barq_mfp | Ref,RelCont |
// CHECK: [[@LINE+1]]:19 | type-alias/associated-type/Swift | Assoc | s:14swift_ide_test2P1P5AssocQa | Ref,RelCont |
  func bar(x: Bar.Assoc) {}
}

// MARK: - Test extending a generic type in a generic context

// CHECK: [[@LINE+1]]:15 | type-alias/generic-type-param/Swift | WrapperParam | s:14swift_ide_test7WrapperC0D5Paramxmfp | Def,RelChild |
class Wrapper<WrapperParam> {
// CHECK: [[@LINE+2]]:9 | class/Swift | Wrapped | s:14swift_ide_test7WrapperC7WrappedC | Def,RelChild |
// CHECK: [[@LINE+1]]:29 | type-alias/generic-type-param/Swift | Bar | s:14swift_ide_test7WrapperC7WrappedC3Barqd_0_mfp | Def,RelChild |
  class Wrapped<OtherParam, Bar: P1> {}
}

// MARK: Extension restricted on param of inner type

// CHECK: [[@LINE+1]]:33 | type-alias/generic-type-param/Swift | Bar | s:14swift_ide_test7WrapperC7WrappedC3Barqd_0_mfp | Ref |
extension Wrapper.Wrapped where Bar: P2 {
// CHECK: [[@LINE+1]]:15 | type-alias/generic-type-param/Swift | Bar | s:14swift_ide_test7WrapperC7WrappedC3Barqd_0_mfp | Ref,RelCont |
  func foo(x: Bar) {}

// CHECK: [[@LINE+2]]:15 | type-alias/generic-type-param/Swift | Bar | s:14swift_ide_test7WrapperC7WrappedC3Barqd_0_mfp | Ref,RelCont |
// CHECK: [[@LINE+1]]:19 | type-alias/associated-type/Swift | Assoc | s:14swift_ide_test2P1P5AssocQa | Ref,RelCont |
  func bar(x: Bar.Assoc) {}
}

// MARK: Extension restricted on generic param of outer type

// CHECK: [[@LINE+1]]:33 | type-alias/generic-type-param/Swift | WrapperParam | s:14swift_ide_test7WrapperC0D5Paramxmfp | Ref |
extension Wrapper.Wrapped where WrapperParam: P2 {
// CHECK: [[@LINE+1]]:15 | type-alias/generic-type-param/Swift | Bar | s:14swift_ide_test7WrapperC7WrappedC3Barqd_0_mfp | Ref,RelCont |
  func foo(x: Bar) {}

// CHECK: [[@LINE+2]]:15 | type-alias/generic-type-param/Swift | Bar | s:14swift_ide_test7WrapperC7WrappedC3Barqd_0_mfp | Ref,RelCont |
// CHECK: [[@LINE+1]]:19 | type-alias/associated-type/Swift | Assoc | s:14swift_ide_test2P1P5AssocQa | Ref,RelCont |
  func bar(x: Bar.Assoc) {}
}

// MARK: - Test extening a non-generic type in a generic context

// CHECK: [[@LINE+1]]:16 | type-alias/generic-type-param/Swift | Wrapper2Param | s:14swift_ide_test8Wrapper2C0D5Paramxmfp | Def,RelChild |
class Wrapper2<Wrapper2Param> {
  class NonGenericWrapped {}
}

// CHECK: [[@LINE+1]]:44 | type-alias/generic-type-param/Swift | Wrapper2Param | s:14swift_ide_test8Wrapper2C0D5Paramxmfp | Ref |
extension Wrapper2.NonGenericWrapped where Wrapper2Param: P1 {
// CHECK: [[@LINE+1]]:15 | type-alias/generic-type-param/Swift | Wrapper2Param | s:14swift_ide_test8Wrapper2C0D5Paramxmfp | Ref,RelCont |
  func foo(x: Wrapper2Param) {}

// CHECK: [[@LINE+2]]:15 | type-alias/generic-type-param/Swift | Wrapper2Param | s:14swift_ide_test8Wrapper2C0D5Paramxmfp | Ref,RelCont |
// CHECK: [[@LINE+1]]:29 | type-alias/associated-type/Swift | Assoc | s:14swift_ide_test2P1P5AssocQa | Ref,RelCont |
  func bar(x: Wrapper2Param.Assoc) {}
}

// MARK: - Test extending an unknown type

// Check that we don't crash. We don't expect the generic params to show up in the index.
extension MyUnknownType where Wrapper2Param: P1 {
  func foo(x: Wrapper2Param) {}
}

// MARK: - Test indexing a generic initializer

struct A<T> { // CHECK: [[@LINE]]:8 | struct/Swift | A | [[A_USR:.*]] | Def | rel: 0
  init(value: T) {} // CHECK: [[@LINE]]:3 | constructor/Swift | init(value:) | [[A_init_USR:.*]] | Def,RelChild | rel: 1
}

// CHECK: [[@LINE+2]]:5 | struct/Swift | A | [[A_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+1]]:5 | constructor/Swift | init(value:) | [[A_init_USR]] | Ref,Call | rel: 0
_ = A(value: 1)
// CHECK: [[@LINE+2]]:5 | struct/Swift | A | [[A_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+1]]:7 | constructor/Swift | init(value:) | [[A_init_USR]] | Ref,Call | rel: 0
_ = A.init(value: 1)
// CHECK: [[@LINE+3]]:5 | struct/Swift | A | [[A_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+2]]:5 | constructor/Swift | init(value:) | [[A_init_USR]] | Ref,Call | rel: 0
// CHECK-NEXT: [[@LINE+1]]:7 | struct/Swift | Int | s:Si | Ref | rel: 0
_ = A<Int>(value: 1)
// CHECK: [[@LINE+3]]:5 | struct/Swift | A | [[A_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+2]]:7 | struct/Swift | Int | s:Si | Ref | rel: 0
// CHECK-NEXT: [[@LINE+1]]:12 | constructor/Swift | init(value:) | [[A_init_USR]] | Ref,Call | rel: 0
_ = A<Int>.init(value: 1)

// CHECK: [[@LINE+2]]:6 | struct/Swift | A | [[A_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+1]]:6 | constructor/Swift | init(value:) | [[A_init_USR]] | Ref,Call | rel: 0
_ = (A)(value: 1)
// CHECK: [[@LINE+2]]:7 | struct/Swift | A | [[A_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+1]]:7 | constructor/Swift | init(value:) | [[A_init_USR]] | Ref,Call | rel: 0
_ = ((A))(value: 1)
// CHECK: [[@LINE+2]]:7 | struct/Swift | A | [[A_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+1]]:11 | constructor/Swift | init(value:) | [[A_init_USR]] | Ref,Call | rel: 0
_ = ((A)).init(value: 1)

enum B { // CHECK: [[@LINE]]:6 | enum/Swift | B | [[B_USR:.*]] | Def | rel: 0
  struct Nested<T> { // CHECK: [[@LINE]]:10 | struct/Swift | Nested | [[B_Nested_USR:.*]] | Def,RelChild | rel: 1
    init(value: T) {} // CHECK: [[@LINE]]:5 | constructor/Swift | init(value:) | [[B_Nested_init_USR:.*]] | Def,RelChild | rel: 1
  }
}

// CHECK: [[@LINE+3]]:5 | enum/Swift | B | [[B_USR]] | Ref | rel: 0
// CHECK: [[@LINE+2]]:7 | struct/Swift | Nested | [[B_Nested_USR]] | Ref | rel: 0
// CHECK: [[@LINE+1]]:7 | constructor/Swift | init(value:) | [[B_Nested_init_USR]] | Ref,Call | rel: 0
_ = B.Nested(value: 1)
// CHECK-NEXT: [[@LINE+4]]:5 | enum/Swift | B | [[B_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+3]]:7 | struct/Swift | Nested | [[B_Nested_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+2]]:7 | constructor/Swift | init(value:) | [[B_Nested_init_USR]] | Ref,Call | rel: 0
// CHECK-NEXT: [[@LINE+1]]:14 | struct/Swift | Int | s:Si | Ref | rel: 0
_ = B.Nested<Int>(value: 1)
// CHECK-NEXT: [[@LINE+4]]:5 | enum/Swift | B | [[B_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+3]]:7 | struct/Swift | Nested | [[B_Nested_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+2]]:14 | struct/Swift | Int | s:Si | Ref | rel: 0
// CHECK-NEXT: [[@LINE+1]]:19 | constructor/Swift | init(value:) | [[B_Nested_init_USR]] | Ref,Call | rel: 0
_ = B.Nested<Int>.init(value: 1)

// CHECK-NEXT: [[@LINE+4]]:7 | enum/Swift | B | [[B_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+3]]:9 | struct/Swift | Nested | [[B_Nested_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+2]]:9 | constructor/Swift | init(value:) | [[B_Nested_init_USR]] | Ref,Call | rel: 0
// CHECK-NEXT: [[@LINE+1]]:16 | struct/Swift | Int | s:Si | Ref | rel: 0
_ = ((B.Nested<Int>))(value: 1)
// CHECK-NEXT: [[@LINE+4]]:7 | enum/Swift | B | [[B_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+3]]:9 | struct/Swift | Nested | [[B_Nested_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+2]]:16 | struct/Swift | Int | s:Si | Ref | rel: 0
// CHECK-NEXT: [[@LINE+1]]:23 | constructor/Swift | init(value:) | [[B_Nested_init_USR]] | Ref,Call | rel: 0
_ = ((B.Nested<Int>)).init(value: 1)

enum C { // CHECK: [[@LINE]]:6 | enum/Swift | C | [[C_USR:.*]] | Def | rel: 0
  struct Nested { // CHECK: [[@LINE]]:10 | struct/Swift | Nested | [[C_Nested_USR:.*]] | Def,RelChild | rel: 1
    init(value: Int) {} // CHECK: [[@LINE]]:5 | constructor/Swift | init(value:) | [[C_Nested_init_USR:.*]] | Def,RelChild | rel: 1
  }
}

// CHECK: [[@LINE+3]]:5 | enum/Swift | C | [[C_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+2]]:7 | struct/Swift | Nested | [[C_Nested_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+1]]:7 | constructor/Swift | init(value:) | [[C_Nested_init_USR]] | Ref,Call | rel: 0
_ = C.Nested(value: 1)

// CHECK: [[@LINE+3]]:5 | enum/Swift | C | [[C_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+2]]:7 | struct/Swift | Nested | [[C_Nested_USR]] | Ref | rel: 0
// CHECK-NEXT: [[@LINE+1]]:14 | constructor/Swift | init(value:) | [[C_Nested_init_USR]] | Ref,Call | rel: 0
_ = C.Nested.init(value: 1)
