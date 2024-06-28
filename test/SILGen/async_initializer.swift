// RUN: %target-swift-frontend -emit-silgen %s -module-name initializers -swift-version 5  -disable-availability-checking | %FileCheck %s --enable-var-scope --implicit-check-not=hop_to_executor
// REQUIRES: concurrency

// CHECK:       protocol Person {
// CHECK-NEXT:    init() async
// CHECK-NEXT:  }

// CHECK:       struct MyStruct {
// CHECK-NEXT:    init() async
// CHECK-NEXT:  }

// CHECK:       enum MyEnum {
// CHECK-NEXT:    init() async
// CHECK-NEXT:  }

// CHECK:       actor MyActor {
// CHECK-NEXT:    init() async


// CHECK:       class EarthPerson : Person {
// CHECK-NEXT:    required init() async
// CHECK-NEXT:    init(name: String) async

// CHECK:       @_inheritsConvenienceInitializers class NorthAmericaPerson : EarthPerson {
// CHECK-NEXT:    required init() async
// CHECK-NEXT:    override init(name: String) async

// CHECK:       class Cat {
// CHECK-NEXT:    @MainActor init()
// CHECK-NEXT:    @MainActor init(name: String)

// CHECK:       struct Dog {
// CHECK-NEXT:    @MainActor init() async
// CHECK-NEXT:    @MainActor init(name: String)
// CHECK-NEXT:  }

// CHECK:       enum Birb {
// CHECK-NEXT:    @MainActor init() async
// CHECK-NEXT:    @MainActor init(name: String)
// CHECK-NEXT:  }



protocol Person {
  init() async
}

struct MyStruct {
  // CHECK-DAG: sil hidden [ossa] @$s12initializers8MyStructVACyYacfC : $@convention(method) @async (@thin MyStruct.Type) -> MyStruct
  init() async {}
}

enum MyEnum {
  // CHECK-DAG: sil hidden [ossa] @$s12initializers6MyEnumOACyYacfC : $@convention(method) @async (@thin MyEnum.Type) -> MyEnum
  init() async {}
}

actor MyActor {
  // CHECK-DAG:   sil hidden [ossa] @$s12initializers7MyActorCACyYacfc : $@convention(method) @async (@sil_isolated @owned MyActor) -> @owned MyActor
  init() async {}
}

class EarthPerson : Person {
  // CHECK-DAG: sil hidden [ossa] @$s12initializers11EarthPersonCACyYacfc : $@convention(method) @async (@owned EarthPerson) -> @owned EarthPerson
  required init() async {}

  // CHECK-DAG: sil hidden [ossa] @$s12initializers11EarthPersonC4nameACSS_tYacfc : $@convention(method) @async (@owned String, @owned EarthPerson) -> @owned EarthPerson
  init(name: String) async {}

  // CHECK-DAG: sil private [transparent] [thunk] [ossa] @$s12initializers11EarthPersonCAA0C0A2aDPxyYacfCTW : $@convention(witness_method: Person) @async (@thick EarthPerson.Type) -> @out EarthPerson
}

class NorthAmericaPerson : EarthPerson {
  // CHECK-DAG: sil hidden [ossa] @$s12initializers18NorthAmericaPersonCACyYacfc : $@convention(method) @async (@owned NorthAmericaPerson) -> @owned NorthAmericaPerson
  required init() async { await super.init() }

  // CHECK-DAG: sil hidden [ossa] @$s12initializers18NorthAmericaPersonC4nameACSS_tYacfc : $@convention(method) @async (@owned String, @owned NorthAmericaPerson) -> @owned NorthAmericaPerson
  override init(name: String) async { await super.init(name: name) }
}

////
// check that of global-actor isolation is preserved in async initializers.
// there's also coverage for sync global-actor-isolated initializers

func someAsyncFn() async {}

class Cat {
  // CHECK-LABEL:  sil hidden [ossa] @$s12initializers3CatCACyYacfc : $@convention(method) @async (@owned Cat) -> @owned Cat {
  // CHECK:          hop_to_executor {{%[0-9]+}} : $MainActor
  // CHECK:          {{%[0-9]+}} = function_ref @$s12initializers11someAsyncFnyyYaF : $@convention(thin) @async () -> ()
  // CHECK:          {{%[0-9]+}} = apply {{%[0-9]+}}() : $@convention(thin) @async () -> ()
  // CHECK-NEXT:     hop_to_executor {{%[0-9]+}} : $MainActor
  // CHECK:        } // end sil function '$s12initializers3CatCACyYacfc'
  @MainActor init() async {
    await someAsyncFn()
  }

  @MainActor init(name: String) {}
}

struct Dog {
  // CHECK-LABEL:  sil hidden [ossa] @$s12initializers3DogVACyYacfC : $@convention(method) @async (@thin Dog.Type) -> Dog {
  // CHECK:          hop_to_executor {{%[0-9]+}} : $MainActor
  // CHECK:          {{%[0-9]+}} = function_ref @$s12initializers11someAsyncFnyyYaF : $@convention(thin) @async () -> ()
  // CHECK:          {{%[0-9]+}} = apply {{%[0-9]+}}() : $@convention(thin) @async () -> ()
  // CHECK-NEXT:     hop_to_executor {{%[0-9]+}} : $MainActor
  // CHECK:        } // end sil function '$s12initializers3DogVACyYacfC'
  @MainActor init() async {
    await someAsyncFn()
  }

  @MainActor init(name: String) {}
}

enum Birb {
  // CHECK-LABEL:  sil hidden [ossa] @$s12initializers4BirbOACyYacfC : $@convention(method) @async (@thin Birb.Type) -> Birb {
  // CHECK:          hop_to_executor {{%[0-9]+}} : $MainActor
  // CHECK:          {{%[0-9]+}} = function_ref @$s12initializers11someAsyncFnyyYaF : $@convention(thin) @async () -> ()
  // CHECK:          {{%[0-9]+}} = apply {{%[0-9]+}}() : $@convention(thin) @async () -> ()
  // CHECK-NEXT:     hop_to_executor {{%[0-9]+}} : $MainActor
  // CHECK:        } // end sil function '$s12initializers4BirbOACyYacfC'
  @MainActor init() async {
    await someAsyncFn()
  }

  @MainActor init(name: String) {}
}

// CHECK-LABEL:  sil hidden [ossa] @$s12initializers7makeCatyyYaF : $@convention(thin) @async () -> () {
// CHECK:          [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-NEXT:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK:          hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-NEXT:     {{%[0-9]+}} = apply {{%[0-9]+}}({{%[0-9]+}}, {{%[0-9]+}}) : $@convention(method) (@owned String, @thick Cat.Type) -> @owned Cat
// CHECK:          hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK:        } // end sil function '$s12initializers7makeCatyyYaF'
func makeCat() async {
  _ = await Cat(name: "Socks")
}

// CHECK-LABEL:  sil hidden [ossa] @$s12initializers7makeDogyyYaF : $@convention(thin) @async () -> () {
// CHECK:          [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-NEXT:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK:          hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-NEXT:     {{%[0-9]+}} = apply {{%[0-9]+}}({{%[0-9]+}}, {{%[0-9]+}}) : $@convention(method) (@owned String, @thin Dog.Type) -> Dog
// CHECK:          hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK:        } // end sil function '$s12initializers7makeDogyyYaF'
func makeDog() async {
  _ = await Dog(name: "Lassie")
}

// CHECK-LABEL:  sil hidden [ossa] @$s12initializers8makeBirbyyYaF : $@convention(thin) @async () -> () {
// CHECK:          [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-NEXT:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK:          hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-NEXT:     {{%[0-9]+}} = apply {{%[0-9]+}}({{%[0-9]+}}, {{%[0-9]+}}) : $@convention(method) (@owned String, @thin Birb.Type) -> Birb
// CHECK:          hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK:        } // end sil function '$s12initializers8makeBirbyyYaF'
func makeBirb() async {
  _ = await Birb(name: "Chirpy")
}

actor SomeActor {
  var x: Int = 0

  // NOTE: during SILGen, we don't expect any hop_to_executors in here.
  // The implicit check-not covers that for us. The hops are inserted later.
  init() async {}

  // CHECK-LABEL: sil hidden [ossa] @$s12initializers9SomeActorC10someMethodyyYaF : $@convention(method) @async (@sil_isolated @guaranteed SomeActor) -> () {
  // CHECK:           hop_to_executor {{%[0-9]+}} : $SomeActor
  // CHECK: } // end sil function '$s12initializers9SomeActorC10someMethodyyYaF'
  func someMethod() async {}
}

// CHECK-LABEL: sil hidden [ossa] @$s12initializers9makeActorAA04SomeC0CyYaF : $@convention(thin) @async () -> @owned SomeActor {
// CHECK:         hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK:         [[INIT:%[0-9]+]] = function_ref @$s12initializers9SomeActorCACyYacfC : $@convention(method) @async (@thick SomeActor.Type) -> @owned SomeActor
// CHECK:         {{%[0-9]+}} = apply [[INIT]]({{%[0-9]+}}) : $@convention(method) @async (@thick SomeActor.Type) -> @owned SomeActor
// CHECK:         hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK: } // end sil function '$s12initializers9makeActorAA04SomeC0CyYaF'
@MainActor
func makeActor() async -> SomeActor {
  return await SomeActor()
}

// CHECK-LABEL: sil hidden [ossa] @$s12initializers20makeActorFromGenericAA04SomeC0CyYaF : $@convention(thin) @async () -> @owned SomeActor {
// CHECK:          [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-NEXT:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK:          apply
// CHECK-NEXT:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
func makeActorFromGeneric() async -> SomeActor {
  return await SomeActor()
}

// CHECK-LABEL: sil hidden [ossa] @$s12initializers26callActorMethodFromGeneric1ayAA04SomeC0C_tYaF : $@convention(thin) @async (@guaranteed SomeActor) -> () {
// CHECK:          [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-NEXT:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK:          apply
// CHECK-NEXT:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
func callActorMethodFromGeneric(a: SomeActor) async {
  await a.someMethod()
}

// CHECK-LABEL: sil hidden {{.*}} @$s12initializers15makeActorInTaskyyYaF : $@convention(thin) @async () -> () {
// CHECK:          [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-NEXT:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK:          apply
// CHECK-LABEL: sil private [ossa] @$s12initializers15makeActorInTaskyyYaFAA04SomeC0CyYacfU_ : $@convention(thin) @async @substituted <τ_0_0> (@guaranteed Optional<any Actor>) -> @out τ_0_0 for <SomeActor> {
// CHECK:          [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-NEXT:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK:          apply
// CHECK-NEXT:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
@available(SwiftStdlib 5.1, *)
func makeActorInTask() async {
  Task.detached { await SomeActor() }
}

// CHECK-LABEL: sil hidden {{.*}}  @$s12initializers21callActorMethodInTask1ayAA04SomeC0C_tYaF : $@convention(thin) @async (@guaranteed SomeActor) -> () {
// CHECK:          [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-NEXT:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK:          apply
// CHECK-LABEL: sil private [ossa] @$s12initializers21callActorMethodInTask1ayAA04SomeC0C_tYaFyyYacfU_ : $@convention(thin) @async @substituted <τ_0_0> (@guaranteed Optional<any Actor>, @guaranteed SomeActor) -> @out τ_0_0 for <()> {
// CHECK:          [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-NEXT:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK:          apply
// CHECK-NEXT:     hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
@available(SwiftStdlib 5.1, *)
func callActorMethodInTask(a: SomeActor) async {
  Task.detached { await a.someMethod() }
}
