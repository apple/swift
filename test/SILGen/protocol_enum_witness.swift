// RUN: %target-swift-emit-silgen %s | %FileCheck %s
// RUN: %target-swift-emit-silgen %s -enable-library-evolution | %FileCheck %s --check-prefix=CHECK-LIB

public protocol Foo {
  static var button: Self { get }
}

public enum Bar: Foo {
  case button
}

public protocol AnotherFoo {
  static func bar(arg: Int) -> Self
}

public enum AnotherBar: AnotherFoo {
  case bar(arg: Int)
}

public struct PublicStruct {}

enum InternalEnumWithPublicStruct : Foo {
  case button
  case other(PublicStruct)
}

// CHECK-LABEL: sil shared [transparent] [serialized] [thunk] [ossa] @$s21protocol_enum_witness3BarOAA3FooA2aDP6buttonxvgZTW : $@convention(witness_method: Foo) (@thick Bar.Type) -> @out Bar {
// CHECK: bb0([[BAR:%.*]] : $*Bar, [[BAR_TYPE:%.*]] : $@thick Bar.Type):
// CHECK-NEXT: [[META_TYPE:%.*]] = metatype $@thin Bar.Type
// CHECK: [[REF:%.*]] = function_ref @$s21protocol_enum_witness3BarO6buttonyA2CmF : $@convention(method) (@thin Bar.Type) -> Bar
// CHECK-NEXT: [[RESULT:%.*]] = apply [[REF]]([[META_TYPE]]) : $@convention(method) (@thin Bar.Type) -> Bar
// CHECK-NEXT: store [[RESULT]] to [trivial] [[BAR]] : $*Bar
// CHECK-NEXT: [[TUPLE:%.*]] = tuple ()
// CHECK-NEXT: return [[TUPLE]] : $()
// CHECK-END: }

// CHECK-LABEL: sil shared [transparent] [serialized] [ossa] @$s21protocol_enum_witness3BarO6buttonyA2CmF : $@convention(method) (@thin Bar.Type) -> Bar {
// CHECK: bb0({{%.*}} : $@thin Bar.Type):
// CHECK-NEXT: [[CASE:%.*]] = enum $Bar, #Bar.button!enumelt
// CHECK-NEXT: return [[CASE]] : $Bar
// CHECK-END: }

// CHECK-LABEL: sil shared [transparent] [serialized] [thunk] [ossa] @$s21protocol_enum_witness10AnotherBarOAA0D3FooA2aDP3bar3argxSi_tFZTW : $@convention(witness_method: AnotherFoo) (Int, @thick AnotherBar.Type) -> @out AnotherBar {
// CHECK: bb0([[ANOTHER_BAR:%.*]] : $*AnotherBar, [[INT_ARG:%.*]] : $Int, [[ANOTHER_BAR_TYPE:%.*]] : $@thick AnotherBar.Type):
// CHECK-NEXT: [[META_TYPE:%.*]] = metatype $@thin AnotherBar.Type
// CHECK: [[REF:%.*]] = function_ref @$s21protocol_enum_witness10AnotherBarO3baryACSi_tcACmF : $@convention(method) (Int, @thin AnotherBar.Type) -> AnotherBar
// CHECK-NEXT: [[RESULT:%.*]] = apply [[REF]]([[INT_ARG]], [[META_TYPE]]) : $@convention(method) (Int, @thin AnotherBar.Type) -> AnotherBar
// CHECK-NEXT: store [[RESULT]] to [trivial] [[ANOTHER_BAR]] : $*AnotherBar
// CHECK-NEXT: [[TUPLE:%.*]] = tuple ()
// CHECK-NEXT: return [[TUPLE]] : $()
// CHECK-END: }

// CHECK-LABEL: sil_witness_table [serialized] Bar: Foo module protocol_enum_witness {
// CHECK: method #Foo.button!getter: <Self where Self : Foo> (Self.Type) -> () -> Self : @$s21protocol_enum_witness3BarOAA3FooA2aDP6buttonxvgZTW

// CHECK-LABEL: sil_witness_table [serialized] AnotherBar: AnotherFoo module protocol_enum_witness {
// CHECK: method #AnotherFoo.bar: <Self where Self : AnotherFoo> (Self.Type) -> (Int) -> Self : @$s21protocol_enum_witness10AnotherBarOAA0D3FooA2aDP3bar3argxSi_tFZTW

// -----------------------------------------------------------------------------
// rdar://108001491 (SIL verification failed: Found mutating or consuming use of
//                  an in_guaranteed parameter?!:
//                  !ImmutableAddressUseVerifier().isMutatingOrConsuming(fArg))

public struct EntityIdentifier {
  public let value: Swift.UInt64
}

public protocol ObjectProtocol {
  static func entityReference(_ id: EntityIdentifier) -> Self
}

public enum Object : ObjectProtocol {
  case entityReference(EntityIdentifier)
}

// CHECK-LIB-LABEL: sil private [transparent] [thunk] [ossa] @$s21protocol_enum_witness6ObjectOAA0D8ProtocolA2aDP15entityReferenceyxAA16EntityIdentifierVFZTW : $@convention(witness_method: ObjectProtocol) (@in_guaranteed EntityIdentifier, @thick Object.Type) -> @out Object {
// CHECK-LIB: bb0(%0 : $*Object, %1 : $*EntityIdentifier, %2 : $@thick Object.Type):
// CHECK-LIB: [[TEMP:%.*]] = alloc_stack $EntityIdentifier
// CHECK-LIB: copy_addr %1 to [init] %3 : $*EntityIdentifier
// CHECK-LIB: apply %{{.*}}(%0, [[TEMP]], %{{.*}}) : $@convention(method) (@in EntityIdentifier, @thin Object.Type) -> @out Object
// CHECK-LIB-LABEL: } // end sil function '$s21protocol_enum_witness6ObjectOAA0D8ProtocolA2aDP15entityReferenceyxAA16EntityIdentifierVFZTW'
