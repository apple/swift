// RUN: %target-swift-ide-test -print-module -module-to-print=Constructors -I %S/Inputs/ -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s

// CHECK:      struct ExplicitDefaultConstructor {
// CHECK-NEXT:   init()
// CHECK-NEXT:   var x: Int32
// CHECK-NEXT: }
// CHECK-NEXT: struct ImplicitDefaultConstructor {
// CHECK-NEXT:   init()
// CHECK-NEXT:   init(x: Int32)
// CHECK-NEXT:   var x: Int32
// CHECK-NEXT: }
// CHECK-NEXT: struct MemberOfClassType {
// CHECK-NEXT:   init()
// CHECK-NEXT:   init(member: ImplicitDefaultConstructor)
// CHECK-NEXT:   var member: ImplicitDefaultConstructor
// CHECK-NEXT: }
// CHECK-NEXT: struct DefaultConstructorDeleted {
// CHECK-NEXT:   init(a: UnsafeMutablePointer<Int32>)
// CHECK-NEXT:   var a: UnsafeMutablePointer<Int32>
// CHECK-NEXT: }
// CHECK-NEXT: struct ConstructorWithParam {
// CHECK-NEXT:   init(_ val: Int32)
// CHECK-NEXT:   @available(*, deprecated, message
// CHECK-NEXT:   init()
// CHECK-NEXT:   var x: Int32
// CHECK-NEXT: }
// CHECK-NEXT: struct CopyAndMoveConstructor {
// CHECK-NEXT:   @available(*, deprecated, message
// CHECK-NEXT:   init()
// CHECK-NEXT: }
// CHECK-NEXT: struct Base {
// CHECK-NEXT:   init()
// CHECK-NEXT: }
// CHECK-NEXT: struct ArgType {
// CHECK-NEXT:   init()
// CHECK-NEXT:   init(i: Int32)
// CHECK-NEXT:   var i: Int32
// CHECK-NEXT: }
// CHECK-NEXT: struct HasVirtualBase {
// CHECK-NEXT:   init(_ Arg: ArgType)
// CHECK-NEXT:   var i: Int32
// CHECK-NEXT: }
// CHECK:      struct TemplatedConstructor {
// CHECK-NEXT:   init<T>(_ value: T)
// CHECK-NEXT:   @available(*, deprecated, message
// CHECK-NEXT:   init()
// CHECK-NEXT:   var value: ArgType
// CHECK-NEXT: }
// CHECK:      struct TemplatedConstructorWithExtraArg {
// CHECK-NEXT:   init<T>(_: Int32, _ value: T)
// CHECK-NEXT:   init<T>(_ value: T, _: Int32)
// CHECK-NEXT:   init<T, U>(_ value: T, _ other: U)
// CHECK-NEXT:   @available(*, deprecated, message
// CHECK-NEXT:   init()
// CHECK-NEXT: }
