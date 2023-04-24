// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -parse-as-library -emit-silgen -verify %s
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -parse-as-library -emit-silgen -DSILGEN %s | %FileCheck %s
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -parse-as-library -emit-silgen -DSILGEN %s | %FileCheck -check-prefix=CHECK-SYMB %s

// REQUIRES: concurrency
// REQUIRES: objc_interop

import Foundation

// Fixtures

@globalActor final actor FirstActor {
  static let shared: FirstActor = FirstActor()
}

@globalActor final actor SecondActor {
  static let shared: SecondActor = SecondActor()
}


@FirstActor
func isolatedFunc() {}  // expected-note 11{{calls to global function 'isolatedFunc()' from outside of its actor context are implicitly asynchronous}}

// CHECK-LABEL: @_inheritsConvenienceInitializers @objc class BaseWithNonisolatedDeinit : NSObject {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: BaseWithNonisolatedDeinit.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s21deinit_isolation_objc25BaseWithNonisolatedDeinitCfZ
// CHECK-SYMB: // BaseWithNonisolatedDeinit.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc25BaseWithNonisolatedDeinitCfD : $@convention(method) (@owned BaseWithNonisolatedDeinit) -> () {
@objc class BaseWithNonisolatedDeinit : NSObject {}

// CHECK-LABEL: @_inheritsConvenienceInitializers @objc class BaseWithDeinitIsolatedOnFirstActor : NSObject {
// CHECK: @objc @FirstActor deinit
// CHECK: }
// CHECK-SYMB: BaseWithDeinitIsolatedOnFirstActor.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc34BaseWithDeinitIsolatedOnFirstActorCfZ : $@convention(thin) (@owned BaseWithDeinitIsolatedOnFirstActor) -> () {
// CHECK-SYMB: BaseWithDeinitIsolatedOnFirstActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc34BaseWithDeinitIsolatedOnFirstActorCfD : $@convention(method) (@owned BaseWithDeinitIsolatedOnFirstActor) -> () {
@objc class BaseWithDeinitIsolatedOnFirstActor : NSObject {
    @FirstActor deinit {} // expected-note 2{{overridden declaration is here}}
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @objc class BaseWithDeinitIsolatedOnSecondActor : NSObject {
// CHECK: @objc @SecondActor deinit
// CHECK: }
// CHECK-SYMB: BaseWithDeinitIsolatedOnSecondActor.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc35BaseWithDeinitIsolatedOnSecondActorCfZ : $@convention(thin) (@owned BaseWithDeinitIsolatedOnSecondActor) -> () {
// CHECK-SYMB: BaseWithDeinitIsolatedOnSecondActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc35BaseWithDeinitIsolatedOnSecondActorCfD : $@convention(method) (@owned BaseWithDeinitIsolatedOnSecondActor) -> () {
@objc class BaseWithDeinitIsolatedOnSecondActor : NSObject {
    @SecondActor deinit {} // expected-note 2{{overridden declaration is here}}
}

// MARK: - Part 1 - Actors

// CHECK-LABEL: @_inheritsConvenienceInitializers @objc actor ImplicitDeinitActor : NSObject {
// CHECK: @objc nonisolated deinit
// CHECK: }
// CHECK-SYMB-NOT: ImplicitDeinitActor.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s21deinit_isolation_objc19ImplicitDeinitActorCfZ
// CHECK-SYMB: // ImplicitDeinitActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc19ImplicitDeinitActorCfD : $@convention(method) (@owned ImplicitDeinitActor) -> () {
@objc actor ImplicitDeinitActor : NSObject {
    // nonisolated deinit
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @objc actor ExplicitDeinitActor : NSObject {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB: // ExplicitDeinitActor.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc19ExplicitDeinitActorCfZ : $@convention(thin) (@owned ExplicitDeinitActor) -> () {
// CHECK-SYMB: // ExplicitDeinitActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc19ExplicitDeinitActorCfD : $@convention(method) (@owned ExplicitDeinitActor) -> () {
@objc actor ExplicitDeinitActor : NSObject {
    // self-isolated deinit
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous actor-isolated context}}
#endif
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @objc actor NonisolatedDeinitActor : NSObject {
// CHECK: @objc nonisolated deinit
// CHECK: }
// CHECK-SYMB-NOT: NonisolatedDeinitActor.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s21deinit_isolation_objc22NonisolatedDeinitActorCfZ
// CHECK-SYMB: // NonisolatedDeinitActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc22NonisolatedDeinitActorCfD : $@convention(method) (@owned NonisolatedDeinitActor) -> () {
@objc actor NonisolatedDeinitActor : NSObject {
    // nonisolated deinit
    nonisolated deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @objc actor IsolatedDeinitActor : NSObject {
// CHECK: @objc @FirstActor deinit
// CHECK: }
// CHECK-SYMB: // IsolatedDeinitActor.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc19IsolatedDeinitActorCfZ : $@convention(thin) (@owned IsolatedDeinitActor) -> () {
// CHECK-SYMB: // IsolatedDeinitActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc19IsolatedDeinitActorCfD : $@convention(method) (@owned IsolatedDeinitActor) -> () {
@objc actor IsolatedDeinitActor : NSObject {
    // FirstActor-isolated deinit
    @FirstActor deinit {
        isolatedFunc() // ok
    }
}

// MARK: - Part 2 - Classes
// MARK: - Part 2.1 - Without base class

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor @objc class ImplicitDeinit : NSObject {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ImplicitDeinit.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s21deinit_isolation_objc14ImplicitDeinitCfZ
// CHECK-SYMB: // ImplicitDeinit.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc14ImplicitDeinitCfD : $@convention(method) (@owned ImplicitDeinit) -> () {
@FirstActor
@objc class ImplicitDeinit : NSObject {
    // nonisolated deinit
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor @objc class ExplicitDeinit : NSObject {
// CHECK: @objc @FirstActor deinit
// CHECK: }
// CHECK-SYMB: // ExplicitDeinit.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc14ExplicitDeinitCfZ : $@convention(thin) (@owned ExplicitDeinit) -> () {
// CHECK-SYMB: // ExplicitDeinit.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc14ExplicitDeinitCfD : $@convention(method) (@owned ExplicitDeinit) -> () {
@FirstActor
@objc class ExplicitDeinit : NSObject {
    // FirstActor-isolated deinit
    deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor @objc class NonisolatedDeinit : NSObject {
// CHECK: @objc nonisolated deinit
// CHECK: }
// CHECK-SYMB-NOT: NonisolatedDeinit.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s21deinit_isolation_objc17NonisolatedDeinitCfZ
// CHECK-SYMB: // NonisolatedDeinit.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc17NonisolatedDeinitCfD : $@convention(method) (@owned NonisolatedDeinit) -> () {
@FirstActor
@objc class NonisolatedDeinit : NSObject {
    // nonisolated deinit
    nonisolated deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @objc class IsolatedDeinit : NSObject {
// CHECK: @objc @FirstActor deinit
// CHECK: }
// CHECK-SYMB: // IsolatedDeinit.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc14IsolatedDeinitCfZ : $@convention(thin) (@owned IsolatedDeinit) -> () {
// CHECK-SYMB: // IsolatedDeinit.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc14IsolatedDeinitCfD : $@convention(method) (@owned IsolatedDeinit) -> () {
@objc class IsolatedDeinit : NSObject {
    // FirstActor-isolated deinit
    @FirstActor deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor @objc class DifferentIsolatedDeinit : NSObject {
// CHECK: @objc @SecondActor deinit
// CHECK: }
// CHECK-SYMB: // DifferentIsolatedDeinit.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc23DifferentIsolatedDeinitCfZ : $@convention(thin) (@owned DifferentIsolatedDeinit) -> () {
// CHECK-SYMB: // DifferentIsolatedDeinit.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc23DifferentIsolatedDeinitCfD : $@convention(method) (@owned DifferentIsolatedDeinit) -> () {
@FirstActor
@objc class DifferentIsolatedDeinit : NSObject {
    // SecondActor-isolated deinit
    @SecondActor deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous global actor 'SecondActor'-isolated context}}
#endif
    }
}

// MARK: - Part 2.2 - Base class with nonisolated deinit

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @FirstActor class ImplicitDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ImplicitDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s21deinit_isolation_objc32ImplicitDeinitInheritNonisolatedCfZ
// CHECK-SYMB: // ImplicitDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc32ImplicitDeinitInheritNonisolatedCfD : $@convention(method) (@owned ImplicitDeinitInheritNonisolated) -> () {
@FirstActor
class ImplicitDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'ImplicitDeinitInheritNonisolated' has different actor isolation from nonisolated superclass 'BaseWithNonisolatedDeinit'; this is an error in Swift 6}}

    // nonisolated deinit
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @FirstActor class ExplicitDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: @objc @FirstActor deinit
// CHECK: }
// CHECK-SYMB: // ExplicitDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc32ExplicitDeinitInheritNonisolatedCfZ : $@convention(thin) (@owned ExplicitDeinitInheritNonisolated) -> () {
// CHECK-SYMB: // ExplicitDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc32ExplicitDeinitInheritNonisolatedCfD : $@convention(method) (@owned ExplicitDeinitInheritNonisolated) -> () {
@FirstActor
class ExplicitDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'ExplicitDeinitInheritNonisolated' has different actor isolation from nonisolated superclass 'BaseWithNonisolatedDeinit'; this is an error in Swift 6}}

    // FirstActor-isolated deinit
    deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @FirstActor class NonisolatedDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: @objc nonisolated deinit
// CHECK: }
// CHECK-SYMB-NOT: NonisolatedDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s21deinit_isolation_objc024NonisolatedDeinitInheritD0CfZ
// CHECK-SYMB: // NonisolatedDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc024NonisolatedDeinitInheritD0CfD : $@convention(method) (@owned NonisolatedDeinitInheritNonisolated) -> () {
@FirstActor
class NonisolatedDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'NonisolatedDeinitInheritNonisolated' has different actor isolation from nonisolated superclass 'BaseWithNonisolatedDeinit'; this is an error in Swift 6}}

    // nonisolated deinit
    nonisolated deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class IsolatedDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: @objc @FirstActor deinit
// CHECK: }
// CHECK-SYMB: // IsolatedDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc32IsolatedDeinitInheritNonisolatedCfZ : $@convention(thin) (@owned IsolatedDeinitInheritNonisolated) -> () {
// CHECK-SYMB: // IsolatedDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc32IsolatedDeinitInheritNonisolatedCfD : $@convention(method) (@owned IsolatedDeinitInheritNonisolated) -> () {
class IsolatedDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // FirstActor-isolated deinit
    @FirstActor deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @FirstActor class DifferentIsolatedDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK:   @objc @SecondActor deinit
// CHECK: }
// CHECK-SYMB: // DifferentIsolatedDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc41DifferentIsolatedDeinitInheritNonisolatedCfZ : $@convention(thin) (@owned DifferentIsolatedDeinitInheritNonisolated) -> () {
// CHECK-SYMB: // DifferentIsolatedDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc41DifferentIsolatedDeinitInheritNonisolatedCfD : $@convention(method) (@owned DifferentIsolatedDeinitInheritNonisolated) -> () {
@FirstActor
class DifferentIsolatedDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'DifferentIsolatedDeinitInheritNonisolated' has different actor isolation from nonisolated superclass 'BaseWithNonisolatedDeinit'; this is an error in Swift 6}}

    // SecondActor-isolated deinit
    @SecondActor deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous global actor 'SecondActor'-isolated context}}
#endif
    }
}

// MARK: - Part 2.3 - Base class with isolated deinit

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class ImplicitDeinitInheritIsolated1 : BaseWithDeinitIsolatedOnFirstActor {
// CHECK:   @objc @FirstActor deinit
// CHECK: }
// CHECK-SYMB: // ImplicitDeinitInheritIsolated1.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc30ImplicitDeinitInheritIsolated1CfZ : $@convention(thin) (@owned ImplicitDeinitInheritIsolated1) -> () {
// CHECK-SYMB: // ImplicitDeinitInheritIsolated1.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc30ImplicitDeinitInheritIsolated1CfD : $@convention(method) (@owned ImplicitDeinitInheritIsolated1) -> () {
@FirstActor
class ImplicitDeinitInheritIsolated1: BaseWithDeinitIsolatedOnFirstActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'ImplicitDeinitInheritIsolated1' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnFirstActor'; this is an error in Swift 6}}

    // FirstActor-isolated deinit
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class ExplicitDeinitIsolated1 : BaseWithDeinitIsolatedOnFirstActor {
// CHECK:   @objc @FirstActor deinit
// CHECK: }
// CHECK-SYMB: // ExplicitDeinitIsolated1.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc23ExplicitDeinitIsolated1CfZ : $@convention(thin) (@owned ExplicitDeinitIsolated1) -> () {
// CHECK-SYMB: // ExplicitDeinitIsolated1.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc23ExplicitDeinitIsolated1CfD : $@convention(method) (@owned ExplicitDeinitIsolated1) -> () {
@FirstActor
class ExplicitDeinitIsolated1: BaseWithDeinitIsolatedOnFirstActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'ExplicitDeinitIsolated1' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnFirstActor'; this is an error in Swift 6}}

    // FirstActor-isolated deinit
    deinit {
        isolatedFunc() // ok
    }
}

#if !SILGEN
@FirstActor
class NonisolatedDeinitIsolated1: BaseWithDeinitIsolatedOnFirstActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'NonisolatedDeinitIsolated1' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnFirstActor'; this is an error in Swift 6}}

    // nonisolated deinit
    nonisolated deinit { // expected-error {{nonisolated deinitializer 'deinit' has different actor isolation from global actor 'FirstActor'-isolated overridden declaration}}
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
    }
}
#endif

// CHECK-LABEL: @_inheritsConvenienceInitializers class IsolatedDeinitIsolated1 : BaseWithDeinitIsolatedOnFirstActor {
// CHECK:   @objc @FirstActor deinit
// CHECK: }
// CHECK-SYMB: // IsolatedDeinitIsolated1.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc23IsolatedDeinitIsolated1CfZ : $@convention(thin) (@owned IsolatedDeinitIsolated1) -> () {
// CHECK-SYMB: // IsolatedDeinitIsolated1.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc23IsolatedDeinitIsolated1CfD : $@convention(method) (@owned IsolatedDeinitIsolated1) -> () {
class IsolatedDeinitIsolated1: BaseWithDeinitIsolatedOnFirstActor {
    // FirstActor-isolated deinit
    @FirstActor deinit {
        isolatedFunc() // ok
    }
}

#if !SILGEN
@FirstActor
class DifferentIsolatedDeinitIsolated1: BaseWithDeinitIsolatedOnFirstActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'DifferentIsolatedDeinitIsolated1' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnFirstActor'; this is an error in Swift 6}}

    // error
    @SecondActor deinit { // expected-error {{global actor 'SecondActor'-isolated deinitializer 'deinit' has different actor isolation from global actor 'FirstActor'-isolated overridden declaration}}
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous global actor 'SecondActor'-isolated context}}
    }
}
#endif

// MARK: - Part 2.4 - Base class with isolated deinit with different actor

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class ImplicitDeinitInheritIsolated2 : BaseWithDeinitIsolatedOnSecondActor {
// CHECK:   @objc @SecondActor deinit
// CHECK: }
// CHECK-SYMB: // ImplicitDeinitInheritIsolated2.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc30ImplicitDeinitInheritIsolated2CfZ : $@convention(thin) (@owned ImplicitDeinitInheritIsolated2) -> () {
// CHECK-SYMB: // ImplicitDeinitInheritIsolated2.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc30ImplicitDeinitInheritIsolated2CfD : $@convention(method) (@owned ImplicitDeinitInheritIsolated2) -> () {
@FirstActor
class ImplicitDeinitInheritIsolated2: BaseWithDeinitIsolatedOnSecondActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'ImplicitDeinitInheritIsolated2' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnSecondActor'; this is an error in Swift 6}}

    // SecondActor-isolated deinit
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class ExplicitDeinitIsolated2 : BaseWithDeinitIsolatedOnSecondActor {
// CHECK:   @objc @SecondActor deinit
// CHECK: }
// CHECK-SYMB: // ExplicitDeinitIsolated2.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc23ExplicitDeinitIsolated2CfZ : $@convention(thin) (@owned ExplicitDeinitIsolated2) -> () {
// CHECK-SYMB: // ExplicitDeinitIsolated2.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc23ExplicitDeinitIsolated2CfD : $@convention(method) (@owned ExplicitDeinitIsolated2) -> () {
@FirstActor
class ExplicitDeinitIsolated2: BaseWithDeinitIsolatedOnSecondActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'ExplicitDeinitIsolated2' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnSecondActor'; this is an error in Swift 6}}

    // SecondActor-isolated deinit
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous global actor 'SecondActor'-isolated context}}
#endif
    }
}

#if !SILGEN
@FirstActor
class NonisolatedDeinitIsolated2: BaseWithDeinitIsolatedOnSecondActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'NonisolatedDeinitIsolated2' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnSecondActor'; this is an error in Swift 6}}

    // nonisolated deinit
    nonisolated deinit { // expected-error {{nonisolated deinitializer 'deinit' has different actor isolation from global actor 'SecondActor'-isolated overridden declaration}}
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
    }
}
#endif

#if !SILGEN
class IsolatedDeinitIsolated2: BaseWithDeinitIsolatedOnSecondActor {
    // FirstActor-isolated deinit
    @FirstActor deinit { // expected-error {{global actor 'FirstActor'-isolated deinitializer 'deinit' has different actor isolation from global actor 'SecondActor'-isolated overridden declaration}}
        isolatedFunc() // ok
    }
}
#endif

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class DifferentIsolatedDeinitIsolated2 : BaseWithDeinitIsolatedOnSecondActor {
// CHECK:   @objc @SecondActor deinit
// CHECK: }
// CHECK-SYMB: // DifferentIsolatedDeinitIsolated2.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc32DifferentIsolatedDeinitIsolated2CfZ : $@convention(thin) (@owned DifferentIsolatedDeinitIsolated2) -> () {
// CHECK-SYMB: // DifferentIsolatedDeinitIsolated2.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc32DifferentIsolatedDeinitIsolated2CfD : $@convention(method) (@owned DifferentIsolatedDeinitIsolated2) -> () {
@FirstActor
class DifferentIsolatedDeinitIsolated2: BaseWithDeinitIsolatedOnSecondActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'DifferentIsolatedDeinitIsolated2' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnSecondActor'; this is an error in Swift 6}}

    // SecondActor-isolated deinit
    @SecondActor deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous global actor 'SecondActor'-isolated context}}
#endif
    }
}

