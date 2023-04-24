// RUN: %target-swift-frontend -parse-as-library -emit-silgen -verify %s
// RUN: %target-swift-frontend -parse-as-library -emit-silgen -DSILGEN %s | %FileCheck %s
// RUN: %target-swift-frontend -parse-as-library -emit-silgen -DSILGEN %s | %FileCheck -check-prefix=CHECK-SYMB %s

// Fixtures

@globalActor final actor FirstActor {
  static let shared: FirstActor = FirstActor()
}

@globalActor final actor SecondActor {
  static let shared: SecondActor = SecondActor()
}


@FirstActor
func isolatedFunc() {}  // expected-note 11{{calls to global function 'isolatedFunc()' from outside of its actor context are implicitly asynchronous}}

// CHECK-LABEL: class BaseWithNonisolatedDeinit {
// CHECK: @objc deinit
// CHECK: }
class BaseWithNonisolatedDeinit {}

// CHECK-LABEL: class BaseWithDeinitIsolatedOnFirstActor {
// CHECK: @objc @FirstActor deinit
// CHECK: }
class BaseWithDeinitIsolatedOnFirstActor {
    @FirstActor deinit {} // expected-note 2{{overridden declaration is here}}
}

// CHECK-LABEL: class BaseWithDeinitIsolatedOnSecondActor {
// CHECK: @objc @SecondActor deinit
// CHECK: }
class BaseWithDeinitIsolatedOnSecondActor {
    @SecondActor deinit {} // expected-note 2{{overridden declaration is here}}
}

//
// Part 1 - Actors
//

// CHECK-LABEL: actor ImplicitDeinitActor {
// CHECK: @objc nonisolated deinit
// CHECK: }
actor ImplicitDeinitActor {
    // nonisolated deinit
}

// CHECK-LABEL: actor ExplicitDeinitActor {
// CHECK: @objc deinit
// CHECK: }
actor ExplicitDeinitActor {
    // self-isolated deinit
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous actor-isolated context}}
#endif
    }
}

// CHECK-LABEL: actor NonisolatedDeinitActor {
// CHECK: @objc nonisolated deinit
// CHECK: }
actor NonisolatedDeinitActor {
    // nonisolated deinit
    nonisolated deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

// CHECK-LABEL: actor IsolatedDeinitActor {
// CHECK: @objc @FirstActor deinit
// CHECK: }
actor IsolatedDeinitActor {
    // FirstActor-isolated deinit
    @FirstActor deinit {
        isolatedFunc() // ok
    }
}

// Part 2 - Classes
// Part 2.1 - Without base class

// CHECK-LABEL: @FirstActor class ImplicitDeinit {
// CHECK: @objc deinit
// CHECK: }
@FirstActor
class ImplicitDeinit {
    // nonisolated deinit
}

// CHECK-LABEL: @FirstActor class ExplicitDeinit {
// CHECK: @objc @FirstActor deinit
// CHECK: }
@FirstActor
class ExplicitDeinit {
    // FirstActor-isolated deinit
    deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @FirstActor class NonisolatedDeinit {
// CHECK: @objc nonisolated deinit
// CHECK: }
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
// CHECK: @objc @FirstActor deinit
// CHECK: }
class IsolatedDeinit {
    // FirstActor-isolated deinit
    @FirstActor deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @FirstActor class DifferentIsolatedDeinit {
// CHECK: @objc @SecondActor deinit
// CHECK: }
@FirstActor
class DifferentIsolatedDeinit {
    // SecondActor-isolated deinit
    @SecondActor deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to global actor 'FirstActor'-isolated global function 'isolatedFunc()' in a synchronous global actor 'SecondActor'-isolated context}}
#endif
    }
}

// Part 2.2 - Base class with nonisolated deinit

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class ImplicitDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: @objc deinit
// CHECK: }
@FirstActor
class ImplicitDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'ImplicitDeinitInheritNonisolated' has different actor isolation from nonisolated superclass 'BaseWithNonisolatedDeinit'; this is an error in Swift 6}}

    // nonisolated deinit
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class ExplicitDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: @objc @FirstActor deinit
// CHECK: }
@FirstActor
class ExplicitDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'ExplicitDeinitInheritNonisolated' has different actor isolation from nonisolated superclass 'BaseWithNonisolatedDeinit'; this is an error in Swift 6}}

    // FirstActor-isolated deinit
    deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class NonisolatedDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK: @objc nonisolated deinit
// CHECK: }
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
// CHECK: @objc @FirstActor deinit
// CHECK: }
class IsolatedDeinitInheritNonisolated: BaseWithNonisolatedDeinit {
    // FirstActor-isolated deinit
    @FirstActor deinit {
        isolatedFunc() // ok
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class DifferentIsolatedDeinitInheritNonisolated : BaseWithNonisolatedDeinit {
// CHECK:   @objc @SecondActor deinit
// CHECK: }
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

// Part 2.3 - Base class with isolated deinit

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class ImplicitDeinitInheritIsolated1 : BaseWithDeinitIsolatedOnFirstActor {
// CHECK:   @objc @FirstActor deinit
// CHECK: }
@FirstActor
class ImplicitDeinitInheritIsolated1: BaseWithDeinitIsolatedOnFirstActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'ImplicitDeinitInheritIsolated1' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnFirstActor'; this is an error in Swift 6}}

    // FirstActor-isolated deinit
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class ExplicitDeinitIsolated1 : BaseWithDeinitIsolatedOnFirstActor {
// CHECK:   @objc @FirstActor deinit
// CHECK: }
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

// Part 2.4 - Base class with isolated deinit with different actor

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class ImplicitDeinitInheritIsolated2 : BaseWithDeinitIsolatedOnSecondActor {
// CHECK:   @objc @SecondActor deinit
// CHECK: }
@FirstActor
class ImplicitDeinitInheritIsolated2: BaseWithDeinitIsolatedOnSecondActor {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'ImplicitDeinitInheritIsolated2' has different actor isolation from nonisolated superclass 'BaseWithDeinitIsolatedOnSecondActor'; this is an error in Swift 6}}

    // SecondActor-isolated deinit
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class ExplicitDeinitIsolated2 : BaseWithDeinitIsolatedOnSecondActor {
// CHECK:   @objc @SecondActor deinit
// CHECK: }
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

