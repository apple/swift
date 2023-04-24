// RUN: %target-swift-frontend -disable-availability-checking -parse-as-library -emit-silgen -verify %s
// RUN: %target-swift-frontend -disable-availability-checking -parse-as-library -emit-silgen -DSILGEN %s | %FileCheck %s
// RUN: %target-swift-frontend -disable-availability-checking -parse-as-library -emit-silgen -DSILGEN %s | %FileCheck -check-prefix=CHECK-SYMB %s

// REQUIRES: concurrency

// Fixtures

@globalActor final actor FirstActor {
  static let shared = FirstActor()
}

@globalActor final actor SecondActor {
  static let shared = SecondActor()
}

@globalActor private final actor PrivateActor {
  static let shared = PrivateActor()
}

@FirstActor
func isolatedFunc() {}  // expected-note 15{{calls to global function 'isolatedFunc()' from outside of its actor context are implicitly asynchronous}}

// CHECK-LABEL: class BaseWithNonisolatedDeinit {
// CHECK: {{(@objc )?}}deinit
// CHECK: }
// CHECK-SYMB-NOT: BaseWithNonisolatedDeinit.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s16deinit_isolation25BaseWithNonisolatedDeinitCfZ
// CHECK-SYMB: // BaseWithNonisolatedDeinit.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation25BaseWithNonisolatedDeinitCfD : $@convention(method) (@owned BaseWithNonisolatedDeinit) -> () {
class BaseWithNonisolatedDeinit {}

// CHECK-LABEL: class BaseWithDeinitIsolatedOnFirstActor {
// CHECK: {{(@objc )?}}@FirstActor deinit
// CHECK: }
// CHECK-SYMB: BaseWithDeinitIsolatedOnFirstActor.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation34BaseWithDeinitIsolatedOnFirstActorCfZ : $@convention(thin) (@owned BaseWithDeinitIsolatedOnFirstActor) -> () {
// CHECK-SYMB: BaseWithDeinitIsolatedOnFirstActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation34BaseWithDeinitIsolatedOnFirstActorCfD : $@convention(method) (@owned BaseWithDeinitIsolatedOnFirstActor) -> () {
class BaseWithDeinitIsolatedOnFirstActor {
    @FirstActor deinit {} // expected-note 3{{overridden declaration is here}}
}

@FirstActor
class BaseIsolatedOnFirstActor {}

@SecondActor
class BaseIsolatedOnSecondActor {}

// CHECK-LABEL: class BaseWithDeinitIsolatedOnSecondActor {
// CHECK: {{(@objc )?}}@SecondActor deinit
// CHECK: }
// CHECK-SYMB: BaseWithDeinitIsolatedOnSecondActor.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation35BaseWithDeinitIsolatedOnSecondActorCfZ : $@convention(thin) (@owned BaseWithDeinitIsolatedOnSecondActor) -> () {
// CHECK-SYMB: BaseWithDeinitIsolatedOnSecondActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation35BaseWithDeinitIsolatedOnSecondActorCfD : $@convention(method) (@owned BaseWithDeinitIsolatedOnSecondActor) -> () {
class BaseWithDeinitIsolatedOnSecondActor {
    @SecondActor deinit {} // expected-note 3{{overridden declaration is here}}
}

// MARK: - Part 1 - Actors

// CHECK-LABEL: actor ImplicitDeinitActor {
// CHECK: {{(@objc )?}} deinit
// CHECK: }
// CHECK-SYMB-NOT: ImplicitDeinitActor.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s16deinit_isolation19ImplicitDeinitActorCfZ
// CHECK-SYMB: // ImplicitDeinitActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation19ImplicitDeinitActorCfD : $@convention(method) (@owned ImplicitDeinitActor) -> () {
actor ImplicitDeinitActor {
    // nonisolated deinit
}

// CHECK-LABEL: actor DefaultDeinitActor {
// CHECK: {{(@objc )?}}deinit
// CHECK: }
// CHECK-SYMB-NOT: DefaultDeinitActor.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s16deinit_isolation18DefaultDeinitActorCfZ
// CHECK-SYMB: // DefaultDeinitActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation18DefaultDeinitActorCfD : $@convention(method) (@owned DefaultDeinitActor) -> () {
actor DefaultDeinitActor {
    // self-isolated deinit
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

// CHECK-LABEL: actor PropagatedDeinitActor {
// CHECK: {{(@objc )?}}isolated deinit
// CHECK: }
// CHECK-SYMB: // PropagatedDeinitActor.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation21PropagatedDeinitActorCfZ : $@convention(thin) (@owned PropagatedDeinitActor) -> () {
// CHECK-SYMB: // PropagatedDeinitActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation21PropagatedDeinitActorCfD : $@convention(method) (@owned PropagatedDeinitActor) -> () {
actor PropagatedDeinitActor {
    // self-isolated deinit
    isolated deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous actor-isolated context}}
#endif
    }
}

// CHECK-LABEL: actor NonisolatedDeinitActor {
// CHECK: {{(@objc )?}}nonisolated deinit
// CHECK: }
// CHECK-SYMB-NOT: NonisolatedDeinitActor.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s16deinit_isolation22NonisolatedDeinitActorCfZ
// CHECK-SYMB: // NonisolatedDeinitActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation22NonisolatedDeinitActorCfD : $@convention(method) (@owned NonisolatedDeinitActor) -> () {
actor NonisolatedDeinitActor {
    // nonisolated deinit
    nonisolated deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

// CHECK-LABEL: actor IsolatedDeinitActor {
// CHECK: {{(@objc )?}}@FirstActor deinit
// CHECK: }
// CHECK-SYMB: // IsolatedDeinitActor.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation19IsolatedDeinitActorCfZ : $@convention(thin) (@owned IsolatedDeinitActor) -> () {
// CHECK-SYMB: // IsolatedDeinitActor.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation19IsolatedDeinitActorCfD : $@convention(method) (@owned IsolatedDeinitActor) -> () {
actor IsolatedDeinitActor {
    // FirstActor-isolated deinit
    @FirstActor deinit {
        isolatedFunc() // ok
    }
}

// MARK: - Part 2 - Classes
// MARK: - Part 2.1 - Without base class

// CHECK-LABEL: @FirstActor class ImplicitDeinit {
// CHECK: {{(@objc )?}}deinit
// CHECK: }
// CHECK-SYMB-NOT: ImplicitDeinit.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s16deinit_isolation14ImplicitDeinitCfZ
// CHECK-SYMB: // ImplicitDeinit.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation14ImplicitDeinitCfD : $@convention(method) (@owned ImplicitDeinit) -> () {
@FirstActor
class ImplicitDeinit {
    // nonisolated deinit
}

// CHECK-LABEL: @FirstActor class DefaultDeinit {
// CHECK: {{(@objc )?}} deinit
// CHECK: }
// CHECK-SYMB-NOT: DefaultDeinit.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s16deinit_isolation13DefaultDeinitCfZ
// CHECK-SYMB: // DefaultDeinit.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation13DefaultDeinitCfD : $@convention(method) (@owned DefaultDeinit) -> () {
@FirstActor
class DefaultDeinit {
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

// CHECK-LABEL: @FirstActor class PropagatedDeinit {
// CHECK: {{(@objc )?}}isolated deinit
// CHECK: }
// CHECK-SYMB: // PropagatedDeinit.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation16PropagatedDeinitCfZ : $@convention(thin) (@owned PropagatedDeinit) -> () {
// CHECK-SYMB: // PropagatedDeinit.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation16PropagatedDeinitCfD : $@convention(method) (@owned PropagatedDeinit) -> () {
@FirstActor
class PropagatedDeinit {
    // FirstActor-isolated deinit
    isolated deinit {
        isolatedFunc() // ok
    }
}

#if !SILGEN
class BadPropagatedDeinit {
    isolated deinit {} // expected-error {{deinit is marked isolated, but containing class 'BadPropagatedDeinit' is not isolated to an actor}}
}
#endif

// CHECK-LABEL: @FirstActor class NonisolatedDeinit {
// CHECK: {{(@objc )?}}nonisolated deinit
// CHECK: }
// CHECK-SYMB-NOT: NonisolatedDeinit.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s16deinit_isolation17NonisolatedDeinitCfZ
// CHECK-SYMB: // NonisolatedDeinit.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation17NonisolatedDeinitCfD : $@convention(method) (@owned NonisolatedDeinit) -> () {
@FirstActor
class NonisolatedDeinit {
    // nonisolated deinit
    nonisolated deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

// CHECK-LABEL: class IsolatedDeinit {
// CHECK: {{(@objc )?}}@FirstActor deinit
// CHECK: }
// CHECK-SYMB: // IsolatedDeinit.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation14IsolatedDeinitCfZ : $@convention(thin) (@owned IsolatedDeinit) -> () {
// CHECK-SYMB: // IsolatedDeinit.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation14IsolatedDeinitCfD : $@convention(method) (@owned IsolatedDeinit) -> () {
class IsolatedDeinit {
    // FirstActor-isolated deinit
    @FirstActor deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @FirstActor class DifferentIsolatedDeinit {
// CHECK: {{(@objc )?}}@SecondActor deinit
// CHECK: }
// CHECK-SYMB: // DifferentIsolatedDeinit.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation23DifferentIsolatedDeinitCfZ : $@convention(thin) (@owned DifferentIsolatedDeinit) -> () {
// CHECK-SYMB: // DifferentIsolatedDeinit.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation23DifferentIsolatedDeinitCfD : $@convention(method) (@owned DifferentIsolatedDeinit) -> () {
@FirstActor
class DifferentIsolatedDeinit {
    // SecondActor-isolated deinit
    @SecondActor deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous global actor 'SecondActor'-isolated context}}
#endif
    }
}

// MARK: - Part 2.2 - Base class with nonisolated deinit

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class ImplicitDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: {{(@objc )?}}deinit
// CHECK: }
// CHECK-SYMB-NOT: ImplicitDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s16deinit_isolation32ImplicitDeinitInheritNonisolatedCfZ
// CHECK-SYMB: // ImplicitDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation32ImplicitDeinitInheritNonisolatedCfD : $@convention(method) (@owned ImplicitDeinitInheritNonisolated) -> () {
@FirstActor
class ImplicitDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'ImplicitDeinitInheritNonisolated' has different actor isolation from nonisolated superclass 'BaseWithNonisolatedDeinit'; this is an error in Swift 6}}

    // nonisolated deinit
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class DefaultDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: {{(@objc )?}} deinit
// CHECK: }
// CHECK-SYMB-NOT: // DefaultDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: sil hidden [ossa] @$s16deinit_isolation31DefaultDeinitInheritNonisolatedCfZ : $@convention(thin) (@owned DefaultDeinitInheritNonisolated) -> () {
// CHECK-SYMB: // DefaultDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation31DefaultDeinitInheritNonisolatedCfD : $@convention(method) (@owned DefaultDeinitInheritNonisolated) -> () {
@FirstActor
class DefaultDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'DefaultDeinitInheritNonisolated' has different actor isolation from nonisolated superclass 'BaseWithNonisolatedDeinit'; this is an error in Swift 6}}

    // nonisolated deinit
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

#if !SILGEN
class BadPropagatedDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    isolated deinit {} // expected-error {{deinit is marked isolated, but containing class 'BadPropagatedDeinitInheritNonisolated' is not isolated to an actor}}
}
#endif

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class PropagatedDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: {{(@objc )?}}isolated deinit
// CHECK: }
// CHECK-SYMB: // PropagatedDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation34PropagatedDeinitInheritNonisolatedCfZ : $@convention(thin) (@owned PropagatedDeinitInheritNonisolated) -> () {
// CHECK-SYMB: // PropagatedDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation34PropagatedDeinitInheritNonisolatedCfD : $@convention(method) (@owned PropagatedDeinitInheritNonisolated) -> () {
@FirstActor
class PropagatedDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'PropagatedDeinitInheritNonisolated' has different actor isolation from nonisolated superclass 'BaseWithNonisolatedDeinit'; this is an error in Swift 6}}

    // FirstActor-isolated deinit
    isolated deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class NonisolatedDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: {{(@objc )?}}nonisolated deinit
// CHECK: }
// CHECK-SYMB-NOT: NonisolatedDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s16deinit_isolation024NonisolatedDeinitInheritC0CfZ
// CHECK-SYMB: // NonisolatedDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation024NonisolatedDeinitInheritC0CfD : $@convention(method) (@owned NonisolatedDeinitInheritNonisolated) -> () {
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

// CHECK-LABEL: @_inheritsConvenienceInitializers class IsolatedDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: {{(@objc )?}}@FirstActor deinit
// CHECK: }
// CHECK-SYMB: // IsolatedDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation32IsolatedDeinitInheritNonisolatedCfZ : $@convention(thin) (@owned IsolatedDeinitInheritNonisolated) -> () {
// CHECK-SYMB: // IsolatedDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation32IsolatedDeinitInheritNonisolatedCfD : $@convention(method) (@owned IsolatedDeinitInheritNonisolated) -> () {
class IsolatedDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // FirstActor-isolated deinit
    @FirstActor deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class DifferentIsolatedDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: {{(@objc )?}}@SecondActor deinit
// CHECK: }
// CHECK-SYMB: // DifferentIsolatedDeinitInheritNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation41DifferentIsolatedDeinitInheritNonisolatedCfZ : $@convention(thin) (@owned DifferentIsolatedDeinitInheritNonisolated) -> () {
// CHECK-SYMB: // DifferentIsolatedDeinitInheritNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation41DifferentIsolatedDeinitInheritNonisolatedCfD : $@convention(method) (@owned DifferentIsolatedDeinitInheritNonisolated) -> () {
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
// CHECK: {{(@objc )?}}deinit
// CHECK: }
// CHECK-SYMB: // ImplicitDeinitInheritIsolated1.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation30ImplicitDeinitInheritIsolated1CfZ : $@convention(thin) (@owned ImplicitDeinitInheritIsolated1) -> () {
// CHECK-SYMB: // ImplicitDeinitInheritIsolated1.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation30ImplicitDeinitInheritIsolated1CfD : $@convention(method) (@owned ImplicitDeinitInheritIsolated1) -> () {
@FirstActor
class ImplicitDeinitInheritIsolated1: BaseWithDeinitIsolatedOnFirstActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'ImplicitDeinitInheritIsolated1' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnFirstActor'; this is an error in Swift 6}}

    // FirstActor-isolated deinit
}

#if !SILGEN
@FirstActor
class DefaultDeinitIsolated1: BaseWithDeinitIsolatedOnFirstActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'DefaultDeinitIsolated1' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnFirstActor'; this is an error in Swift 6}}

    // nonisolated deinit
    deinit { // expected-error {{nonisolated deinitializer 'deinit' has different actor isolation from global actor 'FirstActor'-isolated overridden declaration}}
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
    }
}
#endif

#if !SILGEN
class BadPropagatedDeinitIsolated: BaseWithDeinitIsolatedOnFirstActor {
    //unexpected error produced: nonisolated deinitializer 'deinit' has different actor isolation from global actor 'FirstActor'-isolated overridden declaration

    isolated deinit {} // expected-error {{deinit is marked isolated, but containing class 'BadPropagatedDeinitIsolated' is not isolated to an actor}}
}
#endif

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class GoodPropagatedDeinitIsolated1 : BaseIsolatedOnFirstActor {
// CHECK: {{(@objc )?}}isolated deinit
// CHECK: }
// CHECK-SYMB: // GoodPropagatedDeinitIsolated1.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation29GoodPropagatedDeinitIsolated1CfZ : $@convention(thin) (@owned GoodPropagatedDeinitIsolated1) -> () {
// CHECK-SYMB: // GoodPropagatedDeinitIsolated1.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation29GoodPropagatedDeinitIsolated1CfD : $@convention(method) (@owned GoodPropagatedDeinitIsolated1) -> () {
class GoodPropagatedDeinitIsolated1: BaseIsolatedOnFirstActor {
    isolated deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class PropagatedDeinitIsolated1 : BaseWithDeinitIsolatedOnFirstActor {
// CHECK: {{(@objc )?}}isolated deinit
// CHECK: }
// CHECK-SYMB: // PropagatedDeinitIsolated1.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation25PropagatedDeinitIsolated1CfZ : $@convention(thin) (@owned PropagatedDeinitIsolated1) -> () {
// CHECK-SYMB: // PropagatedDeinitIsolated1.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa]  @$s16deinit_isolation25PropagatedDeinitIsolated1CfD : $@convention(method) (@owned PropagatedDeinitIsolated1) -> () {
@FirstActor
class PropagatedDeinitIsolated1: BaseWithDeinitIsolatedOnFirstActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'PropagatedDeinitIsolated1' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnFirstActor'; this is an error in Swift 6}}

    // FirstActor-isolated deinit
    isolated deinit {
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
// CHECK: {{(@objc )?}}@FirstActor deinit
// CHECK: }
// CHECK-SYMB: // IsolatedDeinitIsolated1.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation23IsolatedDeinitIsolated1CfZ : $@convention(thin) (@owned IsolatedDeinitIsolated1) -> () {
// CHECK-SYMB: // IsolatedDeinitIsolated1.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation23IsolatedDeinitIsolated1CfD : $@convention(method) (@owned IsolatedDeinitIsolated1) -> () {
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
// CHECK: {{(@objc )?}} deinit
// CHECK: }
// CHECK-SYMB: // ImplicitDeinitInheritIsolated2.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation30ImplicitDeinitInheritIsolated2CfZ : $@convention(thin) (@owned ImplicitDeinitInheritIsolated2) -> () {
// CHECK-SYMB: // ImplicitDeinitInheritIsolated2.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation30ImplicitDeinitInheritIsolated2CfD : $@convention(method) (@owned ImplicitDeinitInheritIsolated2) -> () {
@FirstActor
class ImplicitDeinitInheritIsolated2: BaseWithDeinitIsolatedOnSecondActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'ImplicitDeinitInheritIsolated2' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnSecondActor'; this is an error in Swift 6}}

    // SecondActor-isolated deinit
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @SecondActor class GoodPropagatedDeinitIsolated2 : BaseIsolatedOnSecondActor {
// CHECK: {{(@objc )?}}isolated deinit
// CHECK: }
// CHECK-SYMB: // GoodPropagatedDeinitIsolated2.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation29GoodPropagatedDeinitIsolated2CfZ : $@convention(thin) (@owned GoodPropagatedDeinitIsolated2) -> () {
// CHECK-SYMB: // GoodPropagatedDeinitIsolated2.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation29GoodPropagatedDeinitIsolated2CfD : $@convention(method) (@owned GoodPropagatedDeinitIsolated2) -> () {
class GoodPropagatedDeinitIsolated2: BaseIsolatedOnSecondActor {
    isolated deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous global actor 'SecondActor'-isolated context}}
#endif
    }
}

#if !SILGEN
@FirstActor
class PropagatedDeinitIsolated2: BaseWithDeinitIsolatedOnSecondActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'PropagatedDeinitIsolated2' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnSecondActor'; this is an error in Swift 6}}

    // SecondActor-isolated deinit
    isolated deinit { // expected-error {{global actor 'FirstActor'-isolated deinitializer 'deinit' has different actor isolation from global actor 'SecondActor'-isolated overridden declaration}}
        isolatedFunc() // ok
    }
}
#endif

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
// CHECK: {{(@objc )?}}@SecondActor deinit
// CHECK: }
// CHECK-SYMB: // DifferentIsolatedDeinitIsolated2.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation32DifferentIsolatedDeinitIsolated2CfZ : $@convention(thin) (@owned DifferentIsolatedDeinitIsolated2) -> () {
// CHECK-SYMB: // DifferentIsolatedDeinitIsolated2.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s16deinit_isolation32DifferentIsolatedDeinitIsolated2CfD : $@convention(method) (@owned DifferentIsolatedDeinitIsolated2) -> () {
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

#if !SILGEN
public class PublicIsolatedOnPrivateActor {
    // TODO: Both should be producing an error
    @PrivateActor public func ababahalamaha() {}
    @PrivateActor deinit {}
}
#endif
