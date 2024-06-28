// RUN: %empty-directory(%t)

// Due to SE427NoInferenceOnExtension not being in production
// REQUIRES: asserts

// RUN: %target-swift-frontend -swift-version 5 -enable-library-evolution -emit-module \
// RUN:     -enable-experimental-feature SuppressedAssociatedTypes \
// RUN:     -enable-experimental-feature NonescapableTypes \
// RUN:     -enable-experimental-feature SE427NoInferenceOnExtension \
// RUN:     -o %t/NoncopyableGenerics_Misc.swiftmodule \
// RUN:     -emit-module-interface-path %t/NoncopyableGenerics_Misc.swiftinterface \
// RUN:     %S/Inputs/NoncopyableGenerics_Misc.swift

// RUN: %target-swift-frontend -swift-version 5 -enable-library-evolution -emit-module \
// RUN:     -enable-experimental-feature SuppressedAssociatedTypes \
// RUN:     -enable-experimental-feature NonescapableTypes \
// RUN:     -enable-experimental-feature SE427NoInferenceOnExtension \
// RUN:     -o %t/Swiftskell.swiftmodule \
// RUN:     -emit-module-interface-path %t/Swiftskell.swiftinterface \
// RUN:     %S/../Inputs/Swiftskell.swift

// Check the interfaces

// RUN: %FileCheck %s --check-prefix=CHECK-MISC < %t/NoncopyableGenerics_Misc.swiftinterface
// RUN: %FileCheck %s < %t/Swiftskell.swiftinterface

// See if we can compile a module through just the interface and typecheck using it.

// RUN: %target-swift-frontend -compile-module-from-interface \
// RUN:     -enable-experimental-feature SuppressedAssociatedTypes \
// RUN:     -enable-experimental-feature NonescapableTypes \
// RUN:     -enable-experimental-feature SE427NoInferenceOnExtension \
// RUN:    %t/NoncopyableGenerics_Misc.swiftinterface -o %t/NoncopyableGenerics_Misc.swiftmodule

// RUN: %target-swift-frontend -compile-module-from-interface \
// RUN:     -enable-experimental-feature SuppressedAssociatedTypes \
// RUN:     -enable-experimental-feature NonescapableTypes \
// RUN:     -enable-experimental-feature SE427NoInferenceOnExtension \
// RUN:    %t/Swiftskell.swiftinterface -o %t/Swiftskell.swiftmodule

// RUN: %target-swift-frontend -emit-silgen -I %t %s \
// RUN:     -enable-experimental-feature SuppressedAssociatedTypes \
// RUN:    -enable-experimental-feature NonescapableTypes \
// RUN:     -enable-experimental-feature SE427NoInferenceOnExtension \
// RUN:    -o %t/final.silgen

// RUN: %FileCheck %s --check-prefix=CHECK-SILGEN < %t/final.silgen



import NoncopyableGenerics_Misc

// CHECK-MISC: public struct _Toys {
// CHECK-MISC: public struct rdar118697289_S1<Element> {
// CHECK-MISC: public struct rdar118697289_S2<Element> {
// CHECK-MISC: public static func allCopyable1<T>(_ a: T, _ b: T) -> T

// CHECK-MISC: public static func allCopyable2<T>(_ s: T) where T : NoncopyableGenerics_Misc._NoCopyP

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public static func oneCopyable1<T, V>(_ s: T, _ v: borrowing V) where T : {{.*}}._NoCopyP, V : ~Copyable

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public static func oneCopyable2<T, V>(_ s: borrowing T, _ v: V) where T : {{.*}}._NoCopyP, T : ~Copyable

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public static func oneCopyable3<T, V>(_ s: borrowing T, _ v: V) where T : {{.*}}._NoCopyP, T : ~Copyable

// CHECK-MISC: public static func basic_some(_ s: some _NoCopyP)

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public static func basic_some_nc(_ s: borrowing some _NoCopyP & ~Copyable)

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public static func oneEscapable<T, V>(_ s: T, _ v: V) where T : NoncopyableGenerics_Misc._NoEscapableP, T : ~Escapable

// CHECK-MISC: public static func canEscapeButConforms<T>(_ t: T) where T : {{.*}}._NoEscapableP

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public static func opaqueNonEscapable(_ s: some _NoEscapableP & ~Escapable)

// CHECK-MISC: public static func opaqueEscapable(_ s: some _NoEscapableP)

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public struct ExplicitHello<T> : ~Swift.Copyable where T : ~Copyable {

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: extension {{.*}}.ExplicitHello : Swift.Copyable where T : Swift.Copyable {

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public struct Hello<T> : ~Swift.Copyable, ~Swift.Escapable where T : ~Copyable, T : ~Escapable {

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: extension NoncopyableGenerics_Misc.Hello : Swift.Escapable where T : Swift.Escapable {
// CHECK-MISC-NEXT: }
// CHECK-MISC: #endif

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: extension NoncopyableGenerics_Misc.Hello : Swift.Copyable where T : Swift.Copyable {
// CHECK-MISC-NEXT: }
// CHECK-MISC: #endif

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public protocol TestAssocTypes {
// CHECK-MISC-NEXT:   associatedtype A : {{.*}}._NoCopyP, ~Copyable

// CHECK-MISC: public typealias SomeAlias<G> = {{.*}}.Hello<G>

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public typealias AliasWithInverse<G> = {{.*}}.Hello<G> where G : ~Copyable, G : ~Escapable

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public struct RudePointer<T> : Swift.Copyable where T : ~Copyable {

// CHECK-MISC: noInversesSTART
// CHECK-MISC-NOT: ~
// CHECK-MISC: noInversesEND

// CHECK-MISC: public func checkAnyInv1<Result>(_ t: borrowing Result) where Result : ~Copyable
// CHECK-MISC: public func checkAnyInv2<Result>(_ t: borrowing Result) where Result : ~Copyable, Result : ~Escapable
// CHECK-MISC: public func checkAnyObject<Result>(_ t: Result) where Result : AnyObject

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public struct Outer<A> : ~Swift.Copyable where A : ~Copyable {
// CHECK-MISC-NEXT:   public func innerFn<B>(_ b: borrowing B) where B : ~Copyable
// CHECK-MISC:   public struct InnerStruct<C> : ~Swift.Copyable where C : ~Copyable {
// CHECK-MISC-NEXT:     public func g<D>(_ d: borrowing D) where D : ~Copyable
// CHECK-MISC:   public struct InnerVariation1<D> : ~Swift.Copyable, ~Swift.Escapable where D : ~Copyable
// CHECK-MISC:   public struct InnerVariation2<D> : ~Swift.Copyable, ~Swift.Escapable where D : ~Escapable

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: extension {{.*}}.Outer : Swift.Copyable where A : Swift.Copyable {
// CHECK-MISC: #endif

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: extension {{.*}}.Outer.InnerStruct : Swift.Copyable where A : Swift.Copyable, C : Swift.Copyable {
// CHECK-MISC: #endif

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: extension {{.*}}.Outer.InnerVariation1 : Swift.Copyable where A : Swift.Copyable, D : Swift.Copyable {
// CHECK-MISC: #endif

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: extension {{.*}}.Outer.InnerVariation2 : Swift.Escapable where D : Swift.Escapable {
// CHECK-MISC: #endif

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: extension {{.*}}.Outer.InnerStruct {
// CHECK-MISC-NEXT:   public func hello<T>(_ t: T) where T : ~Escapable
// CHECK-MISC: #endif

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: @_preInverseGenerics public func old_swap<T>(_ a: inout T, _ b: inout T) where T : ~Copyable
// CHECK-MISC-NEXT: #else
// CHECK-MISC-NOT: @_preInverseGenerics
// CHECK-MISC-NEXT: public func old_swap<T>(_ a: inout T, _ b: inout T)
// CHECK-MISC: #endif

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: @_preInverseGenerics public func borrowsNoncopyable<T>(_ t: borrowing T) where T : ~Copyable
// CHECK-MISC-NEXT: #else
// CHECK-MISC-NOT: @_preInverseGenerics
// CHECK-MISC-NEXT: public func borrowsNoncopyable<T>(_ t: T)
// CHECK-MISC-NEXT: #endif

// CHECK-MISC: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public func suppressesNoncopyableGenerics<T>(_ t: borrowing T) where T : ~Copyable
// CHECK-MISC-NEXT: #endif

// CHECK-MISC:      #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public struct LoudlyNC<T> where T : ~Copyable {
// CHECK-MISC-NEXT: }
// CHECK-MISC-NEXT: #endif
// CHECK-MISC-NEXT: public func _indexHumongousDonuts<TTT, T>(_ aggregate: Swift.UnsafePointer<TTT>, _ index: Swift.Int) -> T
// CHECK-MISC-NEXT: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public func referToLoud(_ t: {{.*}}.LoudlyNC<Swift.String>)
// CHECK-MISC-NEXT: #else
// CHECK-MISC-NEXT: public func referToLoud(_ t: {{.*}}.LoudlyNC<Swift.String>)
// CHECK-MISC-NEXT: #endif
// CHECK-MISC-NEXT: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public func referToLoudProperGuarding(_ t: {{.*}}.LoudlyNC<Swift.String>)
// CHECK-MISC-NEXT: #endif
// CHECK-MISC-NEXT: public struct NoCopyPls : ~Swift.Copyable {
// CHECK-MISC-NEXT: }
// CHECK-MISC-NEXT: public func substCopyable(_ t: Swift.String?)
// CHECK-MISC-NEXT: public func substGenericCopyable<T>(_ t: T?)

// NOTE: we really shouldn't be emitting the else branch for the two funcs
// below, since the suppressed version isn't valid. We don't have a good way of
// fixing that right now, either.

// CHECK-MISC-NEXT: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public func substNC(_ t: borrowing {{.*}}.NoCopyPls?)
// CHECK-MISC-NEXT: #else
// CHECK-MISC-NEXT: public func substNC(_ t: borrowing {{.*}}.NoCopyPls?)
// CHECK-MISC-NEXT: #endif
// CHECK-MISC-NEXT: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public func substGenericNC<T>(_ t: borrowing T?) where T : ~Copyable
// CHECK-MISC-NEXT: #else
// CHECK-MISC-NEXT: public func substGenericNC<T>(_ t: borrowing T?)
// CHECK-MISC-NEXT: #endif

// CHECK-MISC:      #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public protocol Publik : ~Copyable {
// CHECK-MISC-NEXT: }
// CHECK-MISC-NEXT: #else
// CHECK-MISC-NEXT: public protocol Publik {
// CHECK-MISC-NEXT: }
// CHECK-MISC-NEXT: #endif
// CHECK-MISC-NEXT: public struct Concrete : ~Copyable {
// CHECK-MISC-NEXT: }
// CHECK-MISC-NEXT: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public struct Generic<T> : ~Copyable where T : {{.*}}.Publik, T : ~Copyable {
// CHECK-MISC-NEXT: }
// CHECK-MISC-NEXT: #else
// CHECK-MISC-NEXT: public struct Generic<T> where T : {{.*}}.Publik {
// CHECK-MISC-NEXT: }
// CHECK-MISC-NEXT: #endif
// CHECK-MISC-NEXT: public struct VeryNested : ~Copyable {
// CHECK-MISC-NEXT: }
// CHECK-MISC-NEXT: public struct Twice : ~Copyable, ~Copyable {
// CHECK-MISC-NEXT: }
// CHECK-MISC-NEXT: public struct RegularTwice : ~Swift.Copyable, ~Swift.Copyable {
// CHECK-MISC-NEXT: }

// CHECK-MISC-NEXT: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-MISC-NEXT: public struct Continuation<T, E> where E : Swift.Error, T : ~Copyable {
// CHECK-MISC-NOT:  ~
// CHECK-MISC:      #endif

// NOTE: below are extensions emitted at the end of NoncopyableGenerics_Misc.swift
// CHECK-MISC: extension {{.*}}.VeryNested : {{.*}}.Publik {}

import Swiftskell

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-NEXT: public protocol Show : ~Copyable {
// CHECK: #endif

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-NEXT: public func print(_ s: borrowing some Show & ~Copyable)
// CHECK: #endif

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-NEXT: public protocol Eq : ~Copyable {
// CHECK: #endif

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-NEXT: extension Swiftskell.Eq where Self : ~Copyable {
// CHECK: #endif

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-NEXT: public enum Either<Success, Failure> : ~Swift.Copyable where Failure : Swift.Error, Success : ~Copyable {
// CHECK: #endif

/// This one is position dependent so we can ensure the associated type was printed correctly.
// CHECK: public protocol Generator : ~Copyable {
// CHECK-NEXT:   associatedtype Element : ~Copyable
// CHECK: #endif

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-NEXT: public enum Pair<L, R> : ~Swift.Copyable where L : ~Copyable, R : ~Copyable {
// CHECK: #endif

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-NEXT: extension Swiftskell.Pair : Swift.Copyable where L : Swift.Copyable, R : Swift.Copyable {
// CHECK: #endif

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-NEXT: public enum Maybe<Wrapped> : ~Swift.Copyable where Wrapped : ~Copyable {
// CHECK: #endif

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-NEXT: extension Swiftskell.Maybe : Swift.Copyable where Wrapped : Swift.Copyable {
// CHECK: #endif

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-NEXT: extension Swiftskell.Maybe : Swiftskell.Show where Wrapped : Swiftskell.Show, Wrapped : ~Copyable {
// CHECK: #endif

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-NEXT: extension Swiftskell.Maybe : Swiftskell.Eq where Wrapped : Swiftskell.Eq, Wrapped : ~Copyable {
// CHECK: #endif

// CHECK: #if compiler(>=5.3) && $NoncopyableGenerics
// CHECK-NEXT: public func isJust<A>(_ m: borrowing Swiftskell.Maybe<A>) -> Swift.Bool where A : ~Copyable
// CHECK: #endif

struct FileDescriptor: ~Copyable, Eq, Show {
  let id: Int

  func show() -> String {
    return "FileDescriptor(id:\(id))"
  }

  static func ==(_ a: borrowing Self, _ b: borrowing Self) -> Bool {
    return a.id == b.id
  }

  mutating func exchangeWith(_ other: inout Self) {
    // CHECK-SILGEN: function_ref @$s24NoncopyableGenerics_Misc8old_swapyyxz_xztlF
    old_swap(&self, &other)
  }
}
