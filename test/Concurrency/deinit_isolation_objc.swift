// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -disable-implicit-string-processing-module-import -disable-availability-checking -parse-as-library -emit-silgen -verify %s
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -disable-implicit-string-processing-module-import -disable-availability-checking -parse-as-library -emit-silgen -DSILGEN %s | %FileCheck %s
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -disable-implicit-string-processing-module-import -disable-availability-checking -parse-as-library -emit-silgen -DSILGEN %s | %FileCheck -check-prefix=CHECK-SYMB %s

// REQUIRES: concurrency
// REQUIRES: objc_interop

import Foundation

// Fixtures

@globalActor final actor FirstActor {
  static let shared = FirstActor()
}

@globalActor final actor SecondActor {
  static let shared = SecondActor()
}


@FirstActor
func isolatedFunc() {}  // expected-note 15{{calls to global function 'isolatedFunc()' from outside of its actor context are implicitly asynchronous}}

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
    @FirstActor deinit {} // expected-note 3{{overridden declaration is here}}
}

@FirstActor
@objc class BaseIsolatedOnFirstActor: NSObject {}

@SecondActor
@objc class BaseIsolatedOnSecondActor: NSObject {}

// CHECK-LABEL: @_inheritsConvenienceInitializers @objc class BaseWithDeinitIsolatedOnSecondActor : NSObject {
// CHECK: @objc @SecondActor deinit
// CHECK: }
// CHECK-SYMB: BaseWithDeinitIsolatedOnSecondActor.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc35BaseWithDeinitIsolatedOnSecondActorCfZ : $@convention(thin) (@owned BaseWithDeinitIsolatedOnSecondActor) -> () {
// CHECK-SYMB: BaseWithDeinitIsolatedOnSecondActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc35BaseWithDeinitIsolatedOnSecondActorCfD : $@convention(method) (@owned BaseWithDeinitIsolatedOnSecondActor) -> () {
@objc class BaseWithDeinitIsolatedOnSecondActor: NSObject {
    @SecondActor deinit {} // expected-note 3{{overridden declaration is here}}
}

// MARK: - Part 1 - Actors

// CHECK-LABEL: @_inheritsConvenienceInitializers @objc actor ImplicitDeinitActor : NSObject {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ImplicitDeinitActor.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s21deinit_isolation_objc19ImplicitDeinitActorCfZ
// CHECK-SYMB: // ImplicitDeinitActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc19ImplicitDeinitActorCfD : $@convention(method) (@owned ImplicitDeinitActor) -> () {
@objc actor ImplicitDeinitActor : NSObject {
    // nonisolated deinit
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @objc actor DefaultDeinitActor : NSObject {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: DefaultDeinitActor.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s21deinit_isolation_objc18DefaultDeinitActorCfZ
// CHECK-SYMB: // DefaultDeinitActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc18DefaultDeinitActorCfD : $@convention(method) (@owned DefaultDeinitActor) -> () {
@objc actor DefaultDeinitActor : NSObject {
    // self-isolated deinit
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @objc actor PropagatedDeinitActor : NSObject {
// CHECK: @objc isolated deinit
// CHECK: }
// CHECK-SYMB: // PropagatedDeinitActor.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc21PropagatedDeinitActorCfZ : $@convention(thin) (@owned PropagatedDeinitActor) -> () {
// CHECK-SYMB: // PropagatedDeinitActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc21PropagatedDeinitActorCfD : $@convention(method) (@owned PropagatedDeinitActor) -> () {
@objc actor PropagatedDeinitActor : NSObject {
    // self-isolated deinit
    isolated deinit {
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

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor @objc class DefaultDeinit : NSObject {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: DefaultDeinit.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s21deinit_isolation_objc13DefaultDeinitCfZ
// CHECK-SYMB: // DefaultDeinit.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc13DefaultDeinitCfD : $@convention(method) (@owned DefaultDeinit) -> () {
@FirstActor
@objc class DefaultDeinit: NSObject {
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor @objc class PropagatedDeinit : NSObject {
// CHECK: @objc isolated deinit
// CHECK: }
// CHECK-SYMB: // PropagatedDeinit.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc16PropagatedDeinitCfZ : $@convention(thin) (@owned PropagatedDeinit) -> () {
// CHECK-SYMB: // PropagatedDeinit.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc16PropagatedDeinitCfD : $@convention(method) (@owned PropagatedDeinit) -> () {
@FirstActor
@objc class PropagatedDeinit: NSObject {
    // FirstActor-isolated deinit
    isolated deinit {
        isolatedFunc() // ok
    }
}

#if !SILGEN
@objc class BadPropagatedDeinit: NSObject {
    isolated deinit {} // expected-error {{deinit is marked isolated, but containing class 'BadPropagatedDeinit' is not isolated to an actor}}
}
#endif

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

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor @objc class ImplicitDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ImplicitDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s21deinit_isolation_objc32ImplicitDeinitInheritNonisolatedCfZ
// CHECK-SYMB: // ImplicitDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc32ImplicitDeinitInheritNonisolatedCfD : $@convention(method) (@owned ImplicitDeinitInheritNonisolated) -> () {
@FirstActor
@objc class ImplicitDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'ImplicitDeinitInheritNonisolated' has different actor isolation from nonisolated superclass 'BaseWithNonisolatedDeinit'; this is an error in Swift 6}}

    // nonisolated deinit
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor @objc class DefaultDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: // DefaultDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: sil hidden [ossa] @$s21deinit_isolation_objc31DefaultDeinitInheritNonisolatedCfZ : $@convention(thin) (@owned DefaultDeinitInheritNonisolated) -> () {
// CHECK-SYMB: // DefaultDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc31DefaultDeinitInheritNonisolatedCfD : $@convention(method) (@owned DefaultDeinitInheritNonisolated) -> () {
@FirstActor
@objc class DefaultDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'DefaultDeinitInheritNonisolated' has different actor isolation from nonisolated superclass 'BaseWithNonisolatedDeinit'; this is an error in Swift 6}}

    // nonisolated deinit
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

#if !SILGEN
@objc class BadPropagatedDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    isolated deinit {} // expected-error {{deinit is marked isolated, but containing class 'BadPropagatedDeinitInheritNonisolated' is not isolated to an actor}}
}
#endif

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor @objc class PropagatedDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: @objc isolated deinit
// CHECK: }
// CHECK-SYMB: // PropagatedDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc34PropagatedDeinitInheritNonisolatedCfZ : $@convention(thin) (@owned PropagatedDeinitInheritNonisolated) -> () {
// CHECK-SYMB: // PropagatedDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc34PropagatedDeinitInheritNonisolatedCfD : $@convention(method) (@owned PropagatedDeinitInheritNonisolated) -> () {
@FirstActor
@objc class PropagatedDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'PropagatedDeinitInheritNonisolated' has different actor isolation from nonisolated superclass 'BaseWithNonisolatedDeinit'; this is an error in Swift 6}}

    // FirstActor-isolated deinit
    isolated deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor @objc class NonisolatedDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: @objc nonisolated deinit
// CHECK: }
// CHECK-SYMB-NOT: NonisolatedDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s21deinit_isolation_objc024NonisolatedDeinitInheritD0CfZ
// CHECK-SYMB: // NonisolatedDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc024NonisolatedDeinitInheritD0CfD : $@convention(method) (@owned NonisolatedDeinitInheritNonisolated) -> () {
@FirstActor
@objc class NonisolatedDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'NonisolatedDeinitInheritNonisolated' has different actor isolation from nonisolated superclass 'BaseWithNonisolatedDeinit'; this is an error in Swift 6}}

    // nonisolated deinit
    nonisolated deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @objc class IsolatedDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: @objc @FirstActor deinit
// CHECK: }
// CHECK-SYMB: // IsolatedDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc32IsolatedDeinitInheritNonisolatedCfZ : $@convention(thin) (@owned IsolatedDeinitInheritNonisolated) -> () {
// CHECK-SYMB: // IsolatedDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc32IsolatedDeinitInheritNonisolatedCfD : $@convention(method) (@owned IsolatedDeinitInheritNonisolated) -> () {
@objc class IsolatedDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // FirstActor-isolated deinit
    @FirstActor deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor @objc class DifferentIsolatedDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: @objc @SecondActor deinit
// CHECK: }
// CHECK-SYMB: // DifferentIsolatedDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc41DifferentIsolatedDeinitInheritNonisolatedCfZ : $@convention(thin) (@owned DifferentIsolatedDeinitInheritNonisolated) -> () {
// CHECK-SYMB: // DifferentIsolatedDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc41DifferentIsolatedDeinitInheritNonisolatedCfD : $@convention(method) (@owned DifferentIsolatedDeinitInheritNonisolated) -> () {
@FirstActor
@objc class DifferentIsolatedDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'DifferentIsolatedDeinitInheritNonisolated' has different actor isolation from nonisolated superclass 'BaseWithNonisolatedDeinit'; this is an error in Swift 6}}

    // SecondActor-isolated deinit
    @SecondActor deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous global actor 'SecondActor'-isolated context}}
#endif
    }
}

// MARK: - Part 2.3 - Base class with isolated deinit

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor @objc class ImplicitDeinitInheritIsolated1 : BaseWithDeinitIsolatedOnFirstActor {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB: // ImplicitDeinitInheritIsolated1.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc30ImplicitDeinitInheritIsolated1CfZ : $@convention(thin) (@owned ImplicitDeinitInheritIsolated1) -> () {
// CHECK-SYMB: // ImplicitDeinitInheritIsolated1.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc30ImplicitDeinitInheritIsolated1CfD : $@convention(method) (@owned ImplicitDeinitInheritIsolated1) -> () {
@FirstActor
@objc class ImplicitDeinitInheritIsolated1: BaseWithDeinitIsolatedOnFirstActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'ImplicitDeinitInheritIsolated1' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnFirstActor'; this is an error in Swift 6}}

    // FirstActor-isolated deinit
}

#if !SILGEN
@FirstActor
@objc class DefaultDeinitIsolated1: BaseWithDeinitIsolatedOnFirstActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'DefaultDeinitIsolated1' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnFirstActor'; this is an error in Swift 6}}

    // nonisolated deinit
    deinit { // expected-error {{nonisolated deinitializer 'deinit' has different actor isolation from global actor 'FirstActor'-isolated overridden declaration}}
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
    }
}
#endif

#if !SILGEN
@objc class BadPropagatedDeinitIsolated: BaseWithDeinitIsolatedOnFirstActor {
    isolated deinit {} // expected-error {{deinit is marked isolated, but containing class 'BadPropagatedDeinitIsolated' is not isolated to an actor}}
}
#endif

// CHECK-LABEL: @_inheritsConvenienceInitializers @objc @FirstActor class GoodPropagatedDeinitIsolated1 : BaseIsolatedOnFirstActor {
// CHECK: @objc isolated deinit
// CHECK: }
// CHECK-SYMB: // GoodPropagatedDeinitIsolated1.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc29GoodPropagatedDeinitIsolated1CfZ : $@convention(thin) (@owned GoodPropagatedDeinitIsolated1) -> () {
// CHECK-SYMB: // GoodPropagatedDeinitIsolated1.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc29GoodPropagatedDeinitIsolated1CfD : $@convention(method) (@owned GoodPropagatedDeinitIsolated1) -> () {
@objc class GoodPropagatedDeinitIsolated1: BaseIsolatedOnFirstActor {
    isolated deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor @objc class PropagatedDeinitIsolated1 : BaseWithDeinitIsolatedOnFirstActor {
// CHECK: @objc isolated deinit
// CHECK: }
// CHECK-SYMB: // PropagatedDeinitIsolated1.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc25PropagatedDeinitIsolated1CfZ : $@convention(thin) (@owned PropagatedDeinitIsolated1) -> () {
// CHECK-SYMB: // PropagatedDeinitIsolated1.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa]  @$s21deinit_isolation_objc25PropagatedDeinitIsolated1CfD : $@convention(method) (@owned PropagatedDeinitIsolated1) -> () {
@FirstActor
@objc class PropagatedDeinitIsolated1: BaseWithDeinitIsolatedOnFirstActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'PropagatedDeinitIsolated1' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnFirstActor'; this is an error in Swift 6}}

    // FirstActor-isolated deinit
    isolated deinit {
        isolatedFunc() // ok
    }
}

#if !SILGEN
@FirstActor
@objc class NonisolatedDeinitIsolated1: BaseWithDeinitIsolatedOnFirstActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'NonisolatedDeinitIsolated1' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnFirstActor'; this is an error in Swift 6}}

    // nonisolated deinit
    nonisolated deinit { // expected-error {{nonisolated deinitializer 'deinit' has different actor isolation from global actor 'FirstActor'-isolated overridden declaration}}
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
    }
}
#endif

// CHECK-LABEL: @_inheritsConvenienceInitializers @objc class IsolatedDeinitIsolated1 : BaseWithDeinitIsolatedOnFirstActor {
// CHECK: @objc @FirstActor deinit
// CHECK: }
// CHECK-SYMB: // IsolatedDeinitIsolated1.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc23IsolatedDeinitIsolated1CfZ : $@convention(thin) (@owned IsolatedDeinitIsolated1) -> () {
// CHECK-SYMB: // IsolatedDeinitIsolated1.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc23IsolatedDeinitIsolated1CfD : $@convention(method) (@owned IsolatedDeinitIsolated1) -> () {
@objc class IsolatedDeinitIsolated1: BaseWithDeinitIsolatedOnFirstActor {
    // FirstActor-isolated deinit
    @FirstActor deinit {
        isolatedFunc() // ok
    }
}

#if !SILGEN
@FirstActor
@objc class DifferentIsolatedDeinitIsolated1: BaseWithDeinitIsolatedOnFirstActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'DifferentIsolatedDeinitIsolated1' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnFirstActor'; this is an error in Swift 6}}

    // error
    @SecondActor deinit { // expected-error {{global actor 'SecondActor'-isolated deinitializer 'deinit' has different actor isolation from global actor 'FirstActor'-isolated overridden declaration}}
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous global actor 'SecondActor'-isolated context}}
    }
}
#endif

// MARK: - Part 2.4 - Base class with isolated deinit with different actor

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor @objc class ImplicitDeinitInheritIsolated2 : BaseWithDeinitIsolatedOnSecondActor {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB: // ImplicitDeinitInheritIsolated2.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc30ImplicitDeinitInheritIsolated2CfZ : $@convention(thin) (@owned ImplicitDeinitInheritIsolated2) -> () {
// CHECK-SYMB: // ImplicitDeinitInheritIsolated2.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc30ImplicitDeinitInheritIsolated2CfD : $@convention(method) (@owned ImplicitDeinitInheritIsolated2) -> () {
@FirstActor
@objc class ImplicitDeinitInheritIsolated2: BaseWithDeinitIsolatedOnSecondActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'ImplicitDeinitInheritIsolated2' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnSecondActor'; this is an error in Swift 6}}

    // SecondActor-isolated deinit
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @objc @SecondActor class GoodPropagatedDeinitIsolated2 : BaseIsolatedOnSecondActor {
// CHECK: @objc isolated deinit
// CHECK: }
// CHECK-SYMB: // GoodPropagatedDeinitIsolated2.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc29GoodPropagatedDeinitIsolated2CfZ : $@convention(thin) (@owned GoodPropagatedDeinitIsolated2) -> () {
// CHECK-SYMB: // GoodPropagatedDeinitIsolated2.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc29GoodPropagatedDeinitIsolated2CfD : $@convention(method) (@owned GoodPropagatedDeinitIsolated2) -> () {
@objc class GoodPropagatedDeinitIsolated2: BaseIsolatedOnSecondActor {
    isolated deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous global actor 'SecondActor'-isolated context}}
#endif
    }
}

#if !SILGEN
@FirstActor
@objc class PropagatedDeinitIsolated2: BaseWithDeinitIsolatedOnSecondActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'PropagatedDeinitIsolated2' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnSecondActor'; this is an error in Swift 6}}

    // SecondActor-isolated deinit
    isolated deinit { // expected-error {{global actor 'FirstActor'-isolated deinitializer 'deinit' has different actor isolation from global actor 'SecondActor'-isolated overridden declaration}}
        isolatedFunc() // ok
    }
}
#endif

#if !SILGEN
@FirstActor
@objc class NonisolatedDeinitIsolated2: BaseWithDeinitIsolatedOnSecondActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'NonisolatedDeinitIsolated2' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnSecondActor'; this is an error in Swift 6}}

    // nonisolated deinit
    nonisolated deinit { // expected-error {{nonisolated deinitializer 'deinit' has different actor isolation from global actor 'SecondActor'-isolated overridden declaration}}
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
    }
}
#endif

#if !SILGEN
@objc class IsolatedDeinitIsolated2: BaseWithDeinitIsolatedOnSecondActor {
    // FirstActor-isolated deinit
    @FirstActor deinit { // expected-error {{global actor 'FirstActor'-isolated deinitializer 'deinit' has different actor isolation from global actor 'SecondActor'-isolated overridden declaration}}
        isolatedFunc() // ok
    }
}
#endif

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor @objc class DifferentIsolatedDeinitIsolated2 : BaseWithDeinitIsolatedOnSecondActor {
// CHECK: @objc @SecondActor deinit
// CHECK: }
// CHECK-SYMB: // DifferentIsolatedDeinitIsolated2.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc32DifferentIsolatedDeinitIsolated2CfZ : $@convention(thin) (@owned DifferentIsolatedDeinitIsolated2) -> () {
// CHECK-SYMB: // DifferentIsolatedDeinitIsolated2.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s21deinit_isolation_objc32DifferentIsolatedDeinitIsolated2CfD : $@convention(method) (@owned DifferentIsolatedDeinitIsolated2) -> () {
@FirstActor
@objc class DifferentIsolatedDeinitIsolated2: BaseWithDeinitIsolatedOnSecondActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'DifferentIsolatedDeinitIsolated2' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnSecondActor'; this is an error in Swift 6}}

    // SecondActor-isolated deinit
    @SecondActor deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous global actor 'SecondActor'-isolated context}}
#endif
    }
}

