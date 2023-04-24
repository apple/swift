// RUN: %target-swift-frontend -disable-availability-checking -parse-as-library -emit-silgen -verify %s
// RUN: %target-swift-frontend -disable-availability-checking -parse-as-library -emit-silgen -DSILGEN %s | %FileCheck %s
// RUN: %target-swift-frontend -disable-availability-checking -parse-as-library -emit-silgen -DSILGEN %s | %FileCheck -check-prefix=CHECK-SYMB %s

// REQUIRES: concurrency

// MARK: - Fixtures

@globalActor final actor FirstActor {
  static let shared = FirstActor()
}

@globalActor final actor SecondActor {
  static let shared = SecondActor()
}

@FirstActor
func isolatedFunc() {}

// CHECK-LABEL: @FirstActor class ClassIsolated {
// CHECK: {{(@objc )?}} @FirstActor deinit async
// CHECK: }

// CHECK-SYMB: // ClassIsolated.deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit13ClassIsolatedCfd : $@convention(method) @async (@guaranteed ClassIsolated) -> @owned Builtin.NativeObject {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $FirstActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s12async_deinit12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB-NEXT: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s12async_deinit13ClassIsolatedCfd'

// CHECK-SYMB: // ClassIsolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit13ClassIsolatedCfZ : $@convention(thin) @async (@owned ClassIsolated) -> () {

// CHECK-SYMB: // ClassIsolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit13ClassIsolatedCfD : $@convention(method) (@owned ClassIsolated) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $FirstActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s12async_deinit13ClassIsolatedCfD'
@FirstActor
class ClassIsolated {
    deinit async {
        isolatedFunc()
    }
}

// CHECK-LABEL: actor ActorIsolated {
// CHECK: {{(@objc )?}} deinit async
// CHECK: }

// CHECK-SYMB: // ActorIsolated.deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit13ActorIsolatedCfd : $@convention(method) @async (@guaranteed ActorIsolated) -> @owned Builtin.NativeObject {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor %0 : $ActorIsolated
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FOO_REF:%.*]] = ref_element_addr %0 : $ActorIsolated, #ActorIsolated.foo
// CHECK-SYMB-NEXT: [[FOO_ACCESS:%.*]] = begin_access [read] [dynamic] [[FOO_REF]] : $*Int
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: end_access [[FOO_ACCESS]]
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s12async_deinit12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $FirstActor
// CHECK-SYMB-NEXT: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor %0 : $ActorIsolated
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[WOOF:%.*]] = class_method %0 : $ActorIsolated, #ActorIsolated.woof : (isolated ActorIsolated) -> () async -> (), $@convention(method) @async (@guaranteed ActorIsolated) -> ()
// CHECK-SYMB-NEXT: apply [[WOOF]](%0) : $@convention(method) @async (@guaranteed ActorIsolated) -> ()
// CHECK-SYMB-NEXT: hop_to_executor %0
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s12async_deinit13ActorIsolatedCfd'

// CHECK-SYMB: // ActorIsolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit13ActorIsolatedCfZ : $@convention(thin) @async (@owned ActorIsolated) -> () {

// CHECK-SYMB: // ActorIsolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit13ActorIsolatedCfD : $@convention(method) (@owned ActorIsolated) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $ActorIsolated
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s12async_deinit13ActorIsolatedCfD'
actor ActorIsolated {
    var foo: Int = 42
    
    func woof() async {}
    
    deinit async {
#if !SILGEN
        // expected-warning@+1 {{no 'async' operations occur within 'await' expression}}
        print(await foo)
#endif
        // ok
        print(foo)

        await isolatedFunc()
        
        await woof()
    }
}

// CHECK-LABEL: class AsyncBase {
// CHECK: {{(@objc )?}} deinit async
// CHECK: }

// CHECK-SYMB: // AsyncBase.deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit9AsyncBaseCfd : $@convention(method) @async (@guaranteed AsyncBase) -> @owned Builtin.NativeObject {
// CHECK-SYMB: [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-SYMB-NEXT: hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK-SYMB: apply
// CHECK-SYMB: hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK-SYMB: } // end sil function '$s12async_deinit9AsyncBaseCfd'

// CHECK-SYMB: // AsyncBase.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit9AsyncBaseCfZ : $@convention(thin) @async (@owned AsyncBase) -> () {

// CHECK-SYMB: // AsyncBase.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit9AsyncBaseCfD : $@convention(method) (@owned AsyncBase) -> () {
// CHECK-SYMB: [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[GENERIC_EXEC]]
// CHECK-SYMB: } // end sil function '$s12async_deinit9AsyncBaseCfD'
class AsyncBase {
    deinit async {  // expected-note 2{{async deinit was introduced to class hierarchy here}}
        await Task.yield()
    }
}

class NonisolatedBase {
    deinit {
        print("Boom!")
    }
}

class IsolatedBase {
    @FirstActor deinit {
        print("Boom!")
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers class ImplicitDerived : AsyncBase {
// CHECK: {{(@objc )?}} deinit async
// CHECK: }

// CHECK-SYMB: // ImplicitDerived.deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit15ImplicitDerivedCfd : $@convention(method) @async (@guaranteed ImplicitDerived) -> @owned Builtin.NativeObject {
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = function_ref @$s12async_deinit9AsyncBaseCfd
// CHECK-SYMB-NEXT: apply [[SUPER_DEINIT]]
// CHECK-SYMB: } // end sil function '$s12async_deinit15ImplicitDerivedCfd'

// CHECK-SYMB: // ImplicitDerived.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit15ImplicitDerivedCfZ : $@convention(thin) @async (@owned ImplicitDerived) -> () {

// CHECK-SYMB: // ImplicitDerived.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit15ImplicitDerivedCfD : $@convention(method) (@owned ImplicitDerived) -> () {
class ImplicitDerived: AsyncBase {}

#if !SILGEN
class SyncDerived: AsyncBase {
    deinit {} // expected-error {{deinit must be 'async' because parent class has 'async' deinit}}
}

class IndirectlySyncDerived: ImplicitDerived {
    deinit {} // expected-error {{deinit must be 'async' because parent class has 'async' deinit}}
}
#endif

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class GoodDerived : ImplicitDerived {
// CHECK: {{(@objc )?}} deinit async
// CHECK: }

// CHECK-SYMB: // GoodDerived.deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit11GoodDerivedCfd : $@convention(method) @async (@guaranteed GoodDerived) -> @owned Builtin.NativeObject {
// CHECK-SYMB: [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-SYMB-NEXT: hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK-SYMB: [[REF_FOO:%.*]] = ref_element_addr %0 : $GoodDerived, #GoodDerived.foo
// CHECK-SYMB: hop_to_executor %{{[0-9]+}} : $SecondActor
// CHECK-SYMB: [[REF_FOO_ACCESS:%.*]] = begin_access [read] [dynamic] [[REF_FOO]] : $*Int
// CHECK-SYMB: load [trivial] [[REF_FOO_ACCESS]] : $*Int
// CHECK-SYMB: end_access [[REF_FOO_ACCESS]] : $*Int
// CHECK-SYMB: hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK-SYMB: } // end sil function '$s12async_deinit11GoodDerivedCfd'

// CHECK-SYMB: // GoodDerived.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit11GoodDerivedCfZ : $@convention(thin) @async (@owned GoodDerived) -> () {

// CHECK-SYMB: // GoodDerived.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit11GoodDerivedCfD : $@convention(method) (@owned GoodDerived) -> () {
// CHECK-SYMB: [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[GENERIC_EXEC]]
// CHECK-SYMB: } // end sil function '$s12async_deinit11GoodDerivedCfD'
@FirstActor
class GoodDerived: ImplicitDerived {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'GoodDerived' has different actor isolation from nonisolated superclass 'ImplicitDerived'; this is an error in Swift 6}}

    @SecondActor
    var foo: Int = 42
    
    nonisolated func bar() async {
#if !SILGEN
        // expected-error@+2 {{expression is 'async' but is not marked with 'await'}}
        // expected-note@+1 {{property access is 'async'}}
        print(foo)
#endif
        // ok
        print(await foo)
    }
    
    nonisolated deinit async {
#if !SILGEN
        // expected-error@+2 {{expression is 'async' but is not marked with 'await'}}
        // expected-note@+1 {{property access is 'async'}}
        print(foo)
#endif
        // ok
        print(await foo)
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class IsolatedDerived : ImplicitDerived {
// CHECK: {{(@objc )?}} @FirstActor deinit async
// CHECK: }

// CHECK-SYMB: // IsolatedDerived.deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit15IsolatedDerivedCfd : $@convention(method) @async (@guaranteed IsolatedDerived) -> @owned Builtin.NativeObject {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $FirstActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s12async_deinit15IsolatedDerivedCfd'

// CHECK-SYMB: // IsolatedDerived.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit15IsolatedDerivedCfZ : $@convention(thin) @async (@owned IsolatedDerived) -> () {

// CHECK-SYMB: // IsolatedDerived.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit15IsolatedDerivedCfD : $@convention(method) (@owned IsolatedDerived) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $FirstActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s12async_deinit15IsolatedDerivedCfD'
@FirstActor
class IsolatedDerived: ImplicitDerived {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'IsolatedDerived' has different actor isolation from nonisolated superclass 'ImplicitDerived'; this is an error in Swift 6}}

    var foo: Int = 42 // expected-note {{property declared here}}
#if !SILGEN
    nonisolated func probe() {
        // expected-error@+1 {{global actor 'FirstActor'-isolated property 'foo' can not be referenced from a non-isolated context}}
        print(foo)
    }
#endif
        
    @FirstActor deinit async {
#if !SILGEN
        // expected-warning@+1 {{no 'async' operations occur within 'await' expression}}
        print(await foo)
#endif
        // ok
        print(foo)
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class AnotherIsolated : IsolatedDerived {
// CHECK: {{(@objc )?}} @SecondActor deinit async
// CHECK: }

// CHECK-SYMB: // AnotherIsolated.deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit15AnotherIsolatedCfd : $@convention(method) @async (@guaranteed AnotherIsolated) -> @owned Builtin.NativeObject {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $SecondActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[WOOF:%.*]] = class_method %0 : $AnotherIsolated, #AnotherIsolated.woof : (AnotherIsolated) -> () -> (), $@convention(method) (@guaranteed AnotherIsolated) -> ()
// CHECK-SYMB-NEXT: apply [[WOOF]](%0) : $@convention(method) (@guaranteed AnotherIsolated) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = function_ref @$s12async_deinit15IsolatedDerivedCfd : $@convention(method) @async (@guaranteed IsolatedDerived) -> @owned Builtin.NativeObject
// CHECK-SYMB-NEXT: apply [[SUPER_DEINIT]](
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s12async_deinit15AnotherIsolatedCfd'

// CHECK-SYMB: // AnotherIsolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit15AnotherIsolatedCfZ : $@convention(thin) @async (@owned AnotherIsolated) -> () {

// CHECK-SYMB: // AnotherIsolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit15AnotherIsolatedCfD : $@convention(method) (@owned AnotherIsolated) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $SecondActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s12async_deinit15AnotherIsolatedCfD'
class AnotherIsolated: IsolatedDerived {
    @SecondActor func woof() {}
    @SecondActor deinit async {
        woof()
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @FirstActor class DerivedFromNonisolated : NonisolatedBase {
// CHECK: {{(@objc )?}} @FirstActor deinit async
// CHECK: }

// CHECK-SYMB: // DerivedFromNonisolated.deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit22DerivedFromNonisolatedCfd : $@convention(method) @async (@guaranteed DerivedFromNonisolated) -> @owned Builtin.NativeObject {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $FirstActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[YIELD:%.*]] = function_ref @$sScTss5NeverORszABRs_rlE5yieldyyYaFZ : $@convention(method) @async (@thin Task<Never, Never>.Type) -> ()
// CHECK-SYMB-NEXT: apply [[YIELD]]({{%[0-9]+}}) : $@convention(method) @async (@thin Task<Never, Never>.Type) -> ()
// CHECK-SYMB-NEXT: hop_to_executor {{%[0-9]+}} : $FirstActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = function_ref @$s12async_deinit15NonisolatedBaseCfd : $@convention(method) (@guaranteed NonisolatedBase) -> @owned Builtin.NativeObject
// CHECK-SYMB-NEXT: apply [[SUPER_DEINIT]](
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s12async_deinit22DerivedFromNonisolatedCfd'

// CHECK-SYMB: // DerivedFromNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit22DerivedFromNonisolatedCfZ : $@convention(thin) @async (@owned DerivedFromNonisolated) -> () {

// CHECK-SYMB: // DerivedFromNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit22DerivedFromNonisolatedCfD : $@convention(method) (@owned DerivedFromNonisolated) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $FirstActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s12async_deinit22DerivedFromNonisolatedCfD'
@FirstActor
class DerivedFromNonisolated: NonisolatedBase {
    // expected-warning@-1 {{global actor 'FirstActor'-isolated class 'DerivedFromNonisolated' has different actor isolation from nonisolated superclass 'NonisolatedBase'; this is an error in Swift 6}}

    deinit async {
        isolatedFunc()
        await Task.yield()
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers class DerivedFromIsolated : IsolatedBase {
// CHECK: {{(@objc )?}} deinit async
// CHECK: }

// CHECK-SYMB: // DerivedFromIsolated.deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit19DerivedFromIsolatedCfd : $@convention(method) @async (@guaranteed DerivedFromIsolated) -> @owned Builtin.NativeObject {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $FirstActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[YIELD:%.*]] = function_ref @$sScTss5NeverORszABRs_rlE5yieldyyYaFZ : $@convention(method) @async (@thin Task<Never, Never>.Type) -> ()
// CHECK-SYMB-NEXT: apply [[YIELD]]({{%[0-9]+}}) : $@convention(method) @async (@thin Task<Never, Never>.Type) -> ()
// CHECK-SYMB-NEXT: hop_to_executor {{%[0-9]+}} : $FirstActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = function_ref @$s12async_deinit12IsolatedBaseCfd : $@convention(method) (@guaranteed IsolatedBase) -> @owned Builtin.NativeObject
// CHECK-SYMB-NEXT: apply [[SUPER_DEINIT]](
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s12async_deinit19DerivedFromIsolatedCfd'

// CHECK-SYMB: // DerivedFromIsolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit19DerivedFromIsolatedCfZ : $@convention(thin) @async (@owned DerivedFromIsolated) -> () {

// CHECK-SYMB: // DerivedFromIsolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit19DerivedFromIsolatedCfD : $@convention(method) (@owned DerivedFromIsolated) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $FirstActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s12async_deinit19DerivedFromIsolatedCfD'
class DerivedFromIsolated: IsolatedBase {
    deinit async {
        isolatedFunc()
        await Task.yield()
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers class DerivedFromAnotherIsolated : IsolatedBase {
// CHECK: {{(@objc )?}} @SecondActor deinit async
// CHECK: }

// CHECK-SYMB: // DerivedFromAnotherIsolated.deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit26DerivedFromAnotherIsolatedCfd : $@convention(method) @async (@guaranteed DerivedFromAnotherIsolated) -> @owned Builtin.NativeObject {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $SecondActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s12async_deinit12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $FirstActor
// CHECK-SYMB: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB: [[YIELD:%.*]] = function_ref @$sScTss5NeverORszABRs_rlE5yieldyyYaFZ : $@convention(method) @async (@thin Task<Never, Never>.Type) -> ()
// CHECK-SYMB-NEXT: apply [[YIELD]]({{%[0-9]+}}) : $@convention(method) @async (@thin Task<Never, Never>.Type) -> ()
// CHECK-SYMB-NEXT: hop_to_executor {{%[0-9]+}} : $SecondActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = function_ref @$s12async_deinit12IsolatedBaseCfd : $@convention(method) (@guaranteed IsolatedBase) -> @owned Builtin.NativeObject
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $FirstActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(method) (@guaranteed IsolatedBase) -> @owned Builtin.NativeObject
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s12async_deinit26DerivedFromAnotherIsolatedCfd'

// CHECK-SYMB: // DerivedFromAnotherIsolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit26DerivedFromAnotherIsolatedCfZ : $@convention(thin) @async (@owned DerivedFromAnotherIsolated) -> () {

// CHECK-SYMB: // DerivedFromAnotherIsolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit26DerivedFromAnotherIsolatedCfD : $@convention(method) (@owned DerivedFromAnotherIsolated) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $SecondActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s12async_deinit26DerivedFromAnotherIsolatedCfD'
class DerivedFromAnotherIsolated: IsolatedBase {
    @SecondActor deinit async {
        await isolatedFunc()
        await Task.yield()
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers @SecondActor class DerivedPropagatedFromAnotherIsolated : IsolatedBase {
// CHECK: {{(@objc )?}} @SecondActor deinit async
// CHECK: }

// CHECK-SYMB: // DerivedPropagatedFromAnotherIsolated.deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit36DerivedPropagatedFromAnotherIsolatedCfd : $@convention(method) @async (@guaranteed DerivedPropagatedFromAnotherIsolated) -> @owned Builtin.NativeObject {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $SecondActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s12async_deinit12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $FirstActor
// CHECK-SYMB: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB: [[YIELD:%.*]] = function_ref @$sScTss5NeverORszABRs_rlE5yieldyyYaFZ : $@convention(method) @async (@thin Task<Never, Never>.Type) -> ()
// CHECK-SYMB-NEXT: apply [[YIELD]]({{%[0-9]+}}) : $@convention(method) @async (@thin Task<Never, Never>.Type) -> ()
// CHECK-SYMB-NEXT: hop_to_executor {{%[0-9]+}} : $SecondActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = function_ref @$s12async_deinit12IsolatedBaseCfd : $@convention(method) (@guaranteed IsolatedBase) -> @owned Builtin.NativeObject
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $FirstActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(method) (@guaranteed IsolatedBase) -> @owned Builtin.NativeObject
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s12async_deinit36DerivedPropagatedFromAnotherIsolatedCfd'

// CHECK-SYMB: // DerivedPropagatedFromAnotherIsolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit36DerivedPropagatedFromAnotherIsolatedCfZ : $@convention(thin) @async (@owned DerivedPropagatedFromAnotherIsolated) -> () {

// CHECK-SYMB: // DerivedPropagatedFromAnotherIsolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s12async_deinit36DerivedPropagatedFromAnotherIsolatedCfD : $@convention(method) (@owned DerivedPropagatedFromAnotherIsolated) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $SecondActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s12async_deinit36DerivedPropagatedFromAnotherIsolatedCfD'
@SecondActor
class DerivedPropagatedFromAnotherIsolated: IsolatedBase {
    // expected-warning@-1 {{global actor 'SecondActor'-isolated class 'DerivedPropagatedFromAnotherIsolated' has different actor isolation from nonisolated superclass 'IsolatedBase'; this is an error in Swift 6}}

    deinit async {
        await isolatedFunc()
        await Task.yield()
    }
}
