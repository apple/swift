// RUN: %empty-directory(%t/Frameworks)
// RUN: INPUT_DIR=%S/Inputs
// RUN: cp -R $INPUT_DIR/Alpha.framework %t/Frameworks/
// RUN: %empty-directory(%t/Frameworks/Alpha.framework/Modules/Alpha.swiftmodule)
// RUN: %empty-directory(%t/Frameworks/Alpha.framework/Headers/)
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -parse-as-library -module-name Alpha \
// RUN:  -disable-implicit-string-processing-module-import \
// RUN:  -emit-module -o %t/Frameworks/Alpha.framework/Modules/Alpha.swiftmodule/%module-target-triple.swiftmodule \
// RUN:  -enable-objc-interop -disable-objc-attr-requires-foundation-module \
// RUN:  -emit-objc-header -emit-objc-header-path %t/Frameworks/Alpha.framework/Headers/Alpha-Swift.h $INPUT_DIR/Alpha.swift
// RUN: cp -R $INPUT_DIR/Beta.framework %t/Frameworks/
// RUN: %empty-directory(%t/Frameworks/Beta.framework/Headers/)
// RUN: cp $INPUT_DIR/Beta.h %t/Frameworks/Beta.framework/Headers/Beta.h
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -disable-availability-checking -disable-implicit-string-processing-module-import -typecheck -verify %s -F %t/Frameworks -F %clang-importer-sdk-path/frameworks
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -disable-availability-checking -disable-implicit-string-processing-module-import -parse-as-library -emit-silgen -DSILGEN %s -F %t/Frameworks -F %clang-importer-sdk-path/frameworks | %FileCheck %s
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -disable-availability-checking -disable-implicit-string-processing-module-import -parse-as-library -emit-silgen -DSILGEN %s -F %t/Frameworks -F %clang-importer-sdk-path/frameworks | %FileCheck -check-prefix=CHECK-SYMB %s

// REQUIRES: concurrency
// REQUIRES: objc_interop

// Note: intentionally importing Alpha implicitly
import Beta

@globalActor final actor AnotherActor {
  static let shared = AnotherActor()
}

// MARK: - RoundtripNonisolated

@MainActor
func isolatedFunc() {} // expected-note 15{{calls to global function 'isolatedFunc()' from outside of its actor context are implicitly asynchronous}}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_RoundtripNonisolated : RoundtripNonisolated {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_RoundtripNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test34ProbeImplicit_RoundtripNonisolatedCfZ
// CHECK-SYMB: // ProbeImplicit_RoundtripNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test34ProbeImplicit_RoundtripNonisolatedCfD : $@convention(method) (@owned ProbeImplicit_RoundtripNonisolated) -> () {
class ProbeImplicit_RoundtripNonisolated: RoundtripNonisolated {}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeDefault_RoundtripNonisolated : RoundtripNonisolated {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeDefault_RoundtripNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test33ProbeDefault_RoundtripNonisolatedCfZ
// CHECK-SYMB: // ProbeDefault_RoundtripNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test33ProbeDefault_RoundtripNonisolatedCfD : $@convention(method) (@owned ProbeDefault_RoundtripNonisolated) -> () {
class ProbeDefault_RoundtripNonisolated: RoundtripNonisolated {
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

#if !SILGEN
class ProbeIsolated_RoundtripNonisolated: RoundtripNonisolated {
    isolated deinit { // expected-error {{deinit is marked isolated, but containing class 'ProbeIsolated_RoundtripNonisolated' is not isolated to an actor}}
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
    }
}
#endif

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeGlobal_RoundtripNonisolated : RoundtripNonisolated {
// CHECK: @objc @AnotherActor deinit
// CHECK: }
// CHECK-SYMB: ProbeGlobal_RoundtripNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test32ProbeGlobal_RoundtripNonisolatedCfZ : $@convention(thin) (@owned ProbeGlobal_RoundtripNonisolated) -> () {
// CHECK-SYMB: // ProbeGlobal_RoundtripNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test32ProbeGlobal_RoundtripNonisolatedCfD : $@convention(method) (@owned ProbeGlobal_RoundtripNonisolated) -> () {
class ProbeGlobal_RoundtripNonisolated: RoundtripNonisolated {
    @AnotherActor deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous global actor 'AnotherActor'-isolated context}}
#endif
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeDefaultAsync_RoundtripNonisolated : RoundtripNonisolated {
// CHECK: @objc deinit async
// CHECK: }

// CHECK-SYMB: // ProbeDefaultAsync_RoundtripNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test38ProbeDefaultAsync_RoundtripNonisolatedCfZ : $@convention(thin) @async (@owned ProbeDefaultAsync_RoundtripNonisolated) -> () {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-SYMB-NEXT: hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s4test12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NEXT: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor [[GENERIC_EXEC]]
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = objc_super_method %0 : $ProbeDefaultAsync_RoundtripNonisolated, #RoundtripNonisolated.deinit!deallocator.foreign : (RoundtripNonisolated) -> () -> (), $@convention(objc_method) (RoundtripNonisolated) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(objc_method) (RoundtripNonisolated) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s4test38ProbeDefaultAsync_RoundtripNonisolatedCfZ'

// CHECK-SYMB: // ProbeDefaultAsync_RoundtripNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test38ProbeDefaultAsync_RoundtripNonisolatedCfD : $@convention(method) (@owned ProbeDefaultAsync_RoundtripNonisolated) -> () {
// CHECK-SYMB: [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[GENERIC_EXEC]]
// CHECK-SYMB: } // end sil function '$s4test38ProbeDefaultAsync_RoundtripNonisolatedCfD'

class ProbeDefaultAsync_RoundtripNonisolated: RoundtripNonisolated {
    deinit async {
        await isolatedFunc()
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeGlobalAsync_RoundtripNonisolated : RoundtripNonisolated {
// CHECK: @objc @AnotherActor deinit async
// CHECK: }

// CHECK-SYMB: // ProbeGlobalAsync_RoundtripNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test37ProbeGlobalAsync_RoundtripNonisolatedCfZ : $@convention(thin) @async (@owned ProbeGlobalAsync_RoundtripNonisolated) -> () {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s4test12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NEXT: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = objc_super_method %0 : $ProbeGlobalAsync_RoundtripNonisolated, #RoundtripNonisolated.deinit!deallocator.foreign : (RoundtripNonisolated) -> () -> (), $@convention(objc_method) (RoundtripNonisolated) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(objc_method) (RoundtripNonisolated) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s4test37ProbeGlobalAsync_RoundtripNonisolatedCfZ'

// CHECK-SYMB: // ProbeGlobalAsync_RoundtripNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test37ProbeGlobalAsync_RoundtripNonisolatedCfD : $@convention(method) (@owned ProbeGlobalAsync_RoundtripNonisolated) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s4test37ProbeGlobalAsync_RoundtripNonisolatedCfD'
class ProbeGlobalAsync_RoundtripNonisolated: RoundtripNonisolated {
    @AnotherActor deinit async {
#if SILGEN
        await isolatedFunc()
#else
        // expected-error@+2 {{expression is 'async' but is not marked with 'await'}}
        // expected-note@+1 {{calls to global function 'isolatedFunc()' from outside of its actor context are implicitly asynchronous}}
        isolatedFunc()
#endif
    }
}

// MARK: - RoundtripIsolated

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_RoundtripIsolated : RoundtripIsolated {
// Note: Type-checked as isolated, but no @MainActor attribute, because attributes are not added for overriding members
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB: ProbeImplicit_RoundtripIsolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test31ProbeImplicit_RoundtripIsolatedCfZ : $@convention(thin) (@owned ProbeImplicit_RoundtripIsolated) -> () {
// CHECK-SYMB: // ProbeImplicit_RoundtripIsolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test31ProbeImplicit_RoundtripIsolatedCfD : $@convention(method) (@owned ProbeImplicit_RoundtripIsolated) -> () {
class ProbeImplicit_RoundtripIsolated: RoundtripIsolated {}

#if !SILGEN
class ProbeDefault_RoundtripIsolated: RoundtripIsolated {
    deinit {} // expected-error {{nonisolated deinitializer 'deinit' has different actor isolation from main actor-isolated overridden declaration}}
}
#endif

#if !SILGEN
class ProbeIsolated_RoundtripIsolated: RoundtripIsolated {
    isolated deinit { // expected-error {{deinit is marked isolated, but containing class 'ProbeIsolated_RoundtripIsolated' is not isolated to an actor}}
        isolatedFunc() // ok, isolation of the overridden deinit is used as a recovery strategy
    }
}
#endif

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeGlobal_RoundtripIsolated : RoundtripIsolated {
// CHECK: @objc @MainActor deinit
// CHECK: }
// CHECK-SYMB: ProbeGlobal_RoundtripIsolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test29ProbeGlobal_RoundtripIsolatedCfZ : $@convention(thin) (@owned ProbeGlobal_RoundtripIsolated) -> () {
// CHECK-SYMB: // ProbeGlobal_RoundtripIsolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test29ProbeGlobal_RoundtripIsolatedCfD : $@convention(method) (@owned ProbeGlobal_RoundtripIsolated) -> () {
class ProbeGlobal_RoundtripIsolated: RoundtripIsolated {
#if !SILGEN
    @AnotherActor deinit {} // expected-error {{global actor 'AnotherActor'-isolated deinitializer 'deinit' has different actor isolation from main actor-isolated overridden declaration}}
#else
    @MainActor deinit {}
#endif
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeNonisolatedAsync_RoundtripIsolated : RoundtripIsolated {
// CHECK: {{(@objc )?}} nonisolated deinit async
// CHECK: }

// CHECK-SYMB: // ProbeNonisolatedAsync_RoundtripIsolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test39ProbeNonisolatedAsync_RoundtripIsolatedCfZ : $@convention(thin) @async (@owned ProbeNonisolatedAsync_RoundtripIsolated) -> () {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-SYMB-NEXT: hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s4test12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NEXT: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = function_ref @$s5Alpha17RoundtripIsolatedCfZ : $@convention(thin) (@owned RoundtripIsolated) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(thin) (@owned RoundtripIsolated) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s4test39ProbeNonisolatedAsync_RoundtripIsolatedCfZ'

// CHECK-SYMB: // ProbeNonisolatedAsync_RoundtripIsolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test39ProbeNonisolatedAsync_RoundtripIsolatedCfD : $@convention(method) (@owned ProbeNonisolatedAsync_RoundtripIsolated) -> () {
// CHECK-SYMB: [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[GENERIC_EXEC]]
// CHECK-SYMB: } // end sil function '$s4test39ProbeNonisolatedAsync_RoundtripIsolatedCfD'
class ProbeNonisolatedAsync_RoundtripIsolated: RoundtripIsolated {
    nonisolated deinit async {
        await isolatedFunc()
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeGlobalAsync_RoundtripIsolated : RoundtripIsolated {
// CHECK: @objc @AnotherActor deinit async
// CHECK: }

// CHECK-SYMB: // ProbeGlobalAsync_RoundtripIsolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test34ProbeGlobalAsync_RoundtripIsolatedCfZ : $@convention(thin) @async (@owned ProbeGlobalAsync_RoundtripIsolated) -> () {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s4test12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NEXT: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = function_ref @$s5Alpha17RoundtripIsolatedCfZ : $@convention(thin) (@owned RoundtripIsolated) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(thin) (@owned RoundtripIsolated) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s4test34ProbeGlobalAsync_RoundtripIsolatedCfZ'

// CHECK-SYMB: // ProbeGlobalAsync_RoundtripIsolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test34ProbeGlobalAsync_RoundtripIsolatedCfD : $@convention(method) (@owned ProbeGlobalAsync_RoundtripIsolated) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s4test34ProbeGlobalAsync_RoundtripIsolatedCfD'
class ProbeGlobalAsync_RoundtripIsolated: RoundtripIsolated {
    @AnotherActor deinit async {
#if SILGEN
        await isolatedFunc()
#else
        // expected-error@+2 {{expression is 'async' but is not marked with 'await'}}
        // expected-note@+1 {{calls to global function 'isolatedFunc()' from outside of its actor context are implicitly asynchronous}}
        isolatedFunc()
#endif
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeDefaultAsync_RoundtripIsolated : RoundtripIsolated {
// CHECK: @objc deinit async
// CHECK: }

// CHECK-SYMB: // ProbeDefaultAsync_RoundtripIsolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test35ProbeDefaultAsync_RoundtripIsolatedCfZ : $@convention(thin) @async (@owned ProbeDefaultAsync_RoundtripIsolated) -> () {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s4test12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB-NEXT: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = function_ref @$s5Alpha17RoundtripIsolatedCfZ : $@convention(thin) (@owned RoundtripIsolated) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(thin) (@owned RoundtripIsolated) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s4test35ProbeDefaultAsync_RoundtripIsolatedCfZ'

// CHECK-SYMB: // ProbeDefaultAsync_RoundtripIsolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test35ProbeDefaultAsync_RoundtripIsolatedCfD : $@convention(method) (@owned ProbeDefaultAsync_RoundtripIsolated) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s4test35ProbeDefaultAsync_RoundtripIsolatedCfD'
class ProbeDefaultAsync_RoundtripIsolated: RoundtripIsolated {
    // expected-warning@+1 {{async deinit contains no await statements; consider using isolated sync deinit instead}}
    deinit async {
        isolatedFunc()
    }
}

// MARK: - RoundtripAsync

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_RoundtripAsync : RoundtripAsync {
// CHECK: @objc deinit async
// CHECK: }
// CHECK-SYMB: ProbeImplicit_RoundtripAsync.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test28ProbeImplicit_RoundtripAsyncCfZ : $@convention(thin) @async (@owned ProbeImplicit_RoundtripAsync) -> () {
// CHECK-SYMB: // ProbeImplicit_RoundtripAsync.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test28ProbeImplicit_RoundtripAsyncCfD : $@convention(method) (@owned ProbeImplicit_RoundtripAsync) -> () {
class ProbeImplicit_RoundtripAsync: RoundtripAsync {}

#if !SILGEN
class ProbeDefault_RoundtripAsync: RoundtripAsync {
    // expected-error@+1 {{deinit must be 'async' because parent class has 'async' deinit}}
    deinit {}
}
#endif

#if !SILGEN
class ProbeIsolated_RoundtripAsync: RoundtripAsync {
    // expected-error@+2 {{deinit is marked isolated, but containing class 'ProbeIsolated_RoundtripAsync' is not isolated to an actor}}
    // expected-error@+1 {{deinit must be 'async' because parent class has 'async' deinit}}
    isolated deinit {
        isolatedFunc() // ok, isolation of the overridden deinit is used as a recovery strategy
    }
}
#endif

#if !SILGEN
class ProbeGlobal_RoundtripAsync: RoundtripAsync {
    // expected-error@+1 {{deinit must be 'async' because parent class has 'async' deinit}}
    @AnotherActor deinit {}
}
#endif

class ProbeDefaultAsync_RoundtripAsync: RoundtripAsync {
    deinit async {}
}

#if !SILGEN
class ProbeIsolatedAsync_RoundtripAsync: RoundtripAsync {
    isolated deinit async { // expected-error {{deinit is marked isolated, but containing class 'ProbeIsolatedAsync_RoundtripAsync' is not isolated to an actor}}
        isolatedFunc() // ok, isolation of the overridden deinit is used as a recovery strategy
    }
}
#endif

@AnotherActor
class ProbePropagatedAsync_RoundtripAsync: RoundtripAsync {
    // expected-warning@-1 {{global actor 'AnotherActor'-isolated class 'ProbePropagatedAsync_RoundtripAsync' has different actor isolation from nonisolated superclass 'RoundtripAsync'; this is an error in Swift 6}}

    isolated deinit async {
#if SILGEN
        await isolatedFunc()
#else
        // expected-error@+2 {{expression is 'async' but is not marked with 'await'}}
        // expected-note@+1 {{calls to global function 'isolatedFunc()' from outside of its actor context are implicitly asynchronous}}
        isolatedFunc()
#endif
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeGlobalAsync_RoundtripAsync : RoundtripAsync {
// CHECK: @objc @AnotherActor deinit async
// CHECK: }

// CHECK-SYMB: // ProbeGlobalAsync_RoundtripAsync.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test026ProbeGlobalAsync_RoundtripD0CfZ : $@convention(thin) @async (@owned ProbeGlobalAsync_RoundtripAsync) -> () {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s4test12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NEXT: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = function_ref @$s5Alpha14RoundtripAsyncCfZ : $@convention(thin) @async (@owned RoundtripAsync) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(thin) @async (@owned RoundtripAsync) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s4test026ProbeGlobalAsync_RoundtripD0CfZ'

// CHECK-SYMB: // ProbeGlobalAsync_RoundtripAsync.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test026ProbeGlobalAsync_RoundtripD0CfD : $@convention(method) (@owned ProbeGlobalAsync_RoundtripAsync) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s4test026ProbeGlobalAsync_RoundtripD0CfD'
class ProbeGlobalAsync_RoundtripAsync: RoundtripAsync {
    @AnotherActor deinit async {
#if SILGEN
        await isolatedFunc()
#else
        // expected-error@+2 {{expression is 'async' but is not marked with 'await'}}
        // expected-note@+1 {{calls to global function 'isolatedFunc()' from outside of its actor context are implicitly asynchronous}}
        isolatedFunc()
#endif
    }
}

// MARK: - BaseNonisolated

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_BaseNonisolated : BaseNonisolated {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_BaseNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test29ProbeImplicit_BaseNonisolatedCfZ
// CHECK-SYMB: // ProbeImplicit_BaseNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test29ProbeImplicit_BaseNonisolatedCfD : $@convention(method) (@owned ProbeImplicit_BaseNonisolated) -> () {
class ProbeImplicit_BaseNonisolated: BaseNonisolated {}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeDefault_BaseNonisolated : BaseNonisolated {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeDefault_BaseNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test28ProbeDefault_BaseNonisolatedCfZ
// CHECK-SYMB: // ProbeDefault_BaseNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test28ProbeDefault_BaseNonisolatedCfD : $@convention(method) (@owned ProbeDefault_BaseNonisolated) -> () {
class ProbeDefault_BaseNonisolated: BaseNonisolated {
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

#if !SILGEN
class ProbeIsolated_BaseNonisolated: BaseNonisolated {
    isolated deinit { // expected-error {{deinit is marked isolated, but containing class 'ProbeIsolated_BaseNonisolated' is not isolated to an actor}}
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
    }
}
#endif

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeGlobal_BaseNonisolated : BaseNonisolated {
// CHECK: @objc @AnotherActor deinit
// CHECK: }
// CHECK-SYMB: ProbeGlobal_BaseNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test27ProbeGlobal_BaseNonisolatedCfZ : $@convention(thin) (@owned ProbeGlobal_BaseNonisolated) -> () {
// CHECK-SYMB: // ProbeGlobal_BaseNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test27ProbeGlobal_BaseNonisolatedCfD : $@convention(method) (@owned ProbeGlobal_BaseNonisolated) -> () {
class ProbeGlobal_BaseNonisolated: BaseNonisolated {
    @AnotherActor deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous global actor 'AnotherActor'-isolated context}}
#endif
    }
}

// CHECK-LABEL: @_inheritsConvenienceInitializers class ProbeGlobalAsync_BaseNonisolated : BaseNonisolated {
// CHECK: {{(@objc )?}} @AnotherActor deinit async
// CHECK: }

// CHECK-SYMB: // ProbeGlobalAsync_BaseNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test32ProbeGlobalAsync_BaseNonisolatedCfZ : $@convention(thin) @async (@owned ProbeGlobalAsync_BaseNonisolated) -> () {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s4test12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = objc_super_method %0 : $ProbeGlobalAsync_BaseNonisolated, #BaseNonisolated.deinit!deallocator.foreign : (BaseNonisolated) -> () -> (), $@convention(objc_method) (BaseNonisolated) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(objc_method) (BaseNonisolated) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s4test32ProbeGlobalAsync_BaseNonisolatedCfZ'

// CHECK-SYMB: // ProbeGlobalAsync_BaseNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test32ProbeGlobalAsync_BaseNonisolatedCfD : $@convention(method) (@owned ProbeGlobalAsync_BaseNonisolated) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s4test32ProbeGlobalAsync_BaseNonisolatedCfD'
class ProbeGlobalAsync_BaseNonisolated: BaseNonisolated {
    @AnotherActor deinit async {
#if SILGEN
        await isolatedFunc()
#else
        // expected-error@+2 {{expression is 'async' but is not marked with 'await'}}
        // expected-note@+1 {{calls to global function 'isolatedFunc()' from outside of its actor context are implicitly asynchronous}}
        isolatedFunc()
#endif
    }
}

// MARK: - DerivedNonisolated

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_DerivedNonisolated : DerivedNonisolated {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_DerivedNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test32ProbeImplicit_DerivedNonisolatedCfZ
// CHECK-SYMB: // ProbeImplicit_DerivedNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test32ProbeImplicit_DerivedNonisolatedCfD : $@convention(method) (@owned ProbeImplicit_DerivedNonisolated) -> () {
class ProbeImplicit_DerivedNonisolated: DerivedNonisolated {}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeDefault_DerivedNonisolated : DerivedNonisolated {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeDefault_DerivedNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test31ProbeDefault_DerivedNonisolatedCfZ
// CHECK-SYMB: // ProbeDefault_DerivedNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test31ProbeDefault_DerivedNonisolatedCfD : $@convention(method) (@owned ProbeDefault_DerivedNonisolated) -> () {
class ProbeDefault_DerivedNonisolated: DerivedNonisolated {
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

#if !SILGEN
class ProbeIsolated_DerivedNonisolated: DerivedNonisolated {
    isolated deinit { // expected-error {{deinit is marked isolated, but containing class 'ProbeIsolated_DerivedNonisolated' is not isolated to an actor}}
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
    }
}
#endif

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeGlobal_DerivedNonisolated : DerivedNonisolated {
// CHECK: @objc @AnotherActor deinit
// CHECK: }
// CHECK-SYMB: ProbeGlobal_DerivedNonisolated.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test30ProbeGlobal_DerivedNonisolatedCfZ : $@convention(thin) (@owned ProbeGlobal_DerivedNonisolated) -> () {
// CHECK-SYMB: // ProbeGlobal_DerivedNonisolated.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test30ProbeGlobal_DerivedNonisolatedCfD : $@convention(method) (@owned ProbeGlobal_DerivedNonisolated) -> () {
class ProbeGlobal_DerivedNonisolated: DerivedNonisolated {
    @AnotherActor deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous global actor 'AnotherActor'-isolated context}}
#endif
    }
}

// MARK: - BaseIsolatedClass

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @MainActor class ProbeImplicit_BaseIsolatedClass : BaseIsolatedClass {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_BaseIsolatedClass.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test31ProbeImplicit_BaseIsolatedClassCfZ
// CHECK-SYMB: // ProbeImplicit_BaseIsolatedClass.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test31ProbeImplicit_BaseIsolatedClassCfD : $@convention(method) (@owned ProbeImplicit_BaseIsolatedClass) -> () {
class ProbeImplicit_BaseIsolatedClass: BaseIsolatedClass {}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @MainActor class ProbeDefault_BaseIsolatedClass : BaseIsolatedClass {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeDefault_BaseIsolatedClass.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test30ProbeDefault_BaseIsolatedClassCfZ
// CHECK-SYMB: // ProbeDefault_BaseIsolatedClass.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test30ProbeDefault_BaseIsolatedClassCfD : $@convention(method) (@owned ProbeDefault_BaseIsolatedClass) -> () {
class ProbeDefault_BaseIsolatedClass: BaseIsolatedClass {
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @MainActor class ProbeIsolated_BaseIsolatedClass : BaseIsolatedClass {
// CHECK: @objc isolated deinit
// CHECK: }
// CHECK-SYMB: ProbeIsolated_BaseIsolatedClass.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test018ProbeIsolated_BaseC5ClassCfZ : $@convention(thin) (@owned ProbeIsolated_BaseIsolatedClass) -> () {
// CHECK-SYMB: // ProbeIsolated_BaseIsolatedClass.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test018ProbeIsolated_BaseC5ClassCfD : $@convention(method) (@owned ProbeIsolated_BaseIsolatedClass) -> () {
class ProbeIsolated_BaseIsolatedClass: BaseIsolatedClass {
    isolated deinit {
        isolatedFunc()
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @MainActor class ProbeGlobal_BaseIsolatedClass : BaseIsolatedClass {
// CHECK: @objc @AnotherActor deinit
// CHECK: }
// CHECK-SYMB: ProbeGlobal_BaseIsolatedClass.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test29ProbeGlobal_BaseIsolatedClassCfZ : $@convention(thin) (@owned ProbeGlobal_BaseIsolatedClass) -> () {
// CHECK-SYMB: // ProbeGlobal_BaseIsolatedClass.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test29ProbeGlobal_BaseIsolatedClassCfD : $@convention(method) (@owned ProbeGlobal_BaseIsolatedClass) -> () {
class ProbeGlobal_BaseIsolatedClass: BaseIsolatedClass {
    @AnotherActor deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous global actor 'AnotherActor'-isolated context}}
#endif
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @MainActor class ProbePropagatedAsync_BaseIsolatedClass : BaseIsolatedClass {
// CHECK: {{(@objc )?}} @MainActor deinit async
// CHECK: }

// CHECK-SYMB: // ProbePropagatedAsync_BaseIsolatedClass.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test38ProbePropagatedAsync_BaseIsolatedClassCfZ : $@convention(thin) @async (@owned ProbePropagatedAsync_BaseIsolatedClass) -> () {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s4test12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = objc_super_method %0 : $ProbePropagatedAsync_BaseIsolatedClass, #BaseIsolatedClass.deinit!deallocator.foreign : (BaseIsolatedClass) -> () -> (), $@convention(objc_method) (BaseIsolatedClass) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(objc_method) (BaseIsolatedClass) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s4test38ProbePropagatedAsync_BaseIsolatedClassCfZ'

// CHECK-SYMB: // ProbePropagatedAsync_BaseIsolatedClass.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test38ProbePropagatedAsync_BaseIsolatedClassCfD : $@convention(method) (@owned ProbePropagatedAsync_BaseIsolatedClass) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s4test38ProbePropagatedAsync_BaseIsolatedClassCfD'
class ProbePropagatedAsync_BaseIsolatedClass: BaseIsolatedClass {
    deinit async {
#if SILGEN
        isolatedFunc()
#else
        // expected-warning@+1 {{no 'async' operations occur within 'await' expression}}
        await isolatedFunc()
#endif
    }
}

// MARK: - DerivedIsolatedClass

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @MainActor class ProbeImplicit_DerivedIsolatedClass : DerivedIsolatedClass {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_DerivedIsolatedClass.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test34ProbeImplicit_DerivedIsolatedClassCfZ
// CHECK-SYMB: // ProbeImplicit_DerivedIsolatedClass.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test34ProbeImplicit_DerivedIsolatedClassCfD : $@convention(method) (@owned ProbeImplicit_DerivedIsolatedClass) -> () {
class ProbeImplicit_DerivedIsolatedClass: DerivedIsolatedClass {}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @MainActor class ProbeDefault_DerivedIsolatedClass : DerivedIsolatedClass {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeDefault_DerivedIsolatedClass.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test33ProbeDefault_DerivedIsolatedClassCfZ
// CHECK-SYMB: // ProbeDefault_DerivedIsolatedClass.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test33ProbeDefault_DerivedIsolatedClassCfD : $@convention(method) (@owned ProbeDefault_DerivedIsolatedClass) -> () {
class ProbeDefault_DerivedIsolatedClass: DerivedIsolatedClass {
    deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
#endif
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @MainActor class ProbeIsolated_DerivedIsolatedClass : DerivedIsolatedClass {
// CHECK: @objc isolated deinit
// CHECK: }
// CHECK-SYMB: ProbeIsolated_DerivedIsolatedClass.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test021ProbeIsolated_DerivedC5ClassCfZ : $@convention(thin) (@owned ProbeIsolated_DerivedIsolatedClass) -> () {
// CHECK-SYMB: // ProbeIsolated_DerivedIsolatedClass.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test021ProbeIsolated_DerivedC5ClassCfD : $@convention(method) (@owned ProbeIsolated_DerivedIsolatedClass) -> () {
class ProbeIsolated_DerivedIsolatedClass: DerivedIsolatedClass {
    isolated deinit {
        isolatedFunc()
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @MainActor class ProbeGlobal_DerivedIsolatedClass : DerivedIsolatedClass {
// CHECK: @objc @AnotherActor deinit
// CHECK: }
// CHECK-SYMB: ProbeGlobal_DerivedIsolatedClass.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test32ProbeGlobal_DerivedIsolatedClassCfZ : $@convention(thin) (@owned ProbeGlobal_DerivedIsolatedClass) -> () {
// CHECK-SYMB: // ProbeGlobal_DerivedIsolatedClass.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test32ProbeGlobal_DerivedIsolatedClassCfD : $@convention(method) (@owned ProbeGlobal_DerivedIsolatedClass) -> () {
class ProbeGlobal_DerivedIsolatedClass: DerivedIsolatedClass {
    @AnotherActor deinit {
#if !SILGEN
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous global actor 'AnotherActor'-isolated context}}
#endif
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers @MainActor class ProbePropagatedAsync_DerivedIsolatedClass : DerivedIsolatedClass {
// CHECK: {{(@objc )?}} @MainActor deinit async
// CHECK: }

// CHECK-SYMB: // ProbePropagatedAsync_DerivedIsolatedClass.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test41ProbePropagatedAsync_DerivedIsolatedClassCfZ : $@convention(thin) @async (@owned ProbePropagatedAsync_DerivedIsolatedClass) -> () {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s4test12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = objc_super_method %0 : $ProbePropagatedAsync_DerivedIsolatedClass, #DerivedIsolatedClass.deinit!deallocator.foreign : (DerivedIsolatedClass) -> () -> (), $@convention(objc_method) (DerivedIsolatedClass) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(objc_method) (DerivedIsolatedClass) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s4test41ProbePropagatedAsync_DerivedIsolatedClassCfZ'

// CHECK-SYMB: // ProbePropagatedAsync_DerivedIsolatedClass.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test41ProbePropagatedAsync_DerivedIsolatedClassCfD : $@convention(method) (@owned ProbePropagatedAsync_DerivedIsolatedClass) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s4test41ProbePropagatedAsync_DerivedIsolatedClassCfD'
class ProbePropagatedAsync_DerivedIsolatedClass: DerivedIsolatedClass {
    deinit async {
#if SILGEN
        isolatedFunc()
#else
        // expected-warning@+1 {{no 'async' operations occur within 'await' expression}}
        await isolatedFunc()
#endif
    }
}

// MARK: - BaseIsolatedDealloc

// If isolation was introduced in ObjC code, then we assume that ObjC code also
// overrides retain/release to make sure that dealloc is called on the correct
// executor in the first place.

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_BaseIsolatedDealloc : BaseIsolatedDealloc {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_BaseIsolatedDealloc.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test33ProbeImplicit_BaseIsolatedDeallocCfZ
// CHECK-SYMB: // ProbeImplicit_BaseIsolatedDealloc.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test33ProbeImplicit_BaseIsolatedDeallocCfD : $@convention(method) (@owned ProbeImplicit_BaseIsolatedDealloc) -> () {
class ProbeImplicit_BaseIsolatedDealloc: BaseIsolatedDealloc {}

#if !SILGEN
class ProbeDefault_BaseIsolatedDealloc: BaseIsolatedDealloc {
    deinit { // expected-error {{nonisolated deinitializer 'deinit' has different actor isolation from main actor-isolated overridden declaration}}
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
    }
}
#endif

#if !SILGEN
class ProbeIsolated_BaseIsolatedDealloc: BaseIsolatedDealloc {
    isolated deinit { //expected-error {{deinit is marked isolated, but containing class 'ProbeIsolated_BaseIsolatedDealloc' is not isolated to an actor}}
        isolatedFunc() // ok, isolation of the overridden deinit is used as a recovery strategy
    }
}
#endif

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeGlobal_BaseIsolatedDealloc : BaseIsolatedDealloc {
// CHECK: @objc @MainActor deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeGlobal_BaseIsolatedDealloc.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test31ProbeGlobal_BaseIsolatedDeallocCfZ
// CHECK-SYMB: // ProbeGlobal_BaseIsolatedDealloc.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test31ProbeGlobal_BaseIsolatedDeallocCfD : $@convention(method) (@owned ProbeGlobal_BaseIsolatedDealloc) -> () {
class ProbeGlobal_BaseIsolatedDealloc: BaseIsolatedDealloc {
#if !SILGEN
    @AnotherActor deinit {} // expected-error {{global actor 'AnotherActor'-isolated deinitializer 'deinit' has different actor isolation from main actor-isolated overridden declaration}}
#else
    @MainActor deinit {}
#endif
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeDefaultAsync_BaseIsolatedDealloc : BaseIsolatedDealloc {
// CHECK: {{(@objc )?}} deinit async
// CHECK: }

// CHECK-SYMB: // ProbeDefaultAsync_BaseIsolatedDealloc.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test37ProbeDefaultAsync_BaseIsolatedDeallocCfZ : $@convention(thin) @async (@owned ProbeDefaultAsync_BaseIsolatedDealloc) -> () {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s4test12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = objc_super_method %0 : $ProbeDefaultAsync_BaseIsolatedDealloc, #BaseIsolatedDealloc.deinit!deallocator.foreign : (BaseIsolatedDealloc) -> () -> (), $@convention(objc_method) (BaseIsolatedDealloc) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(objc_method) (BaseIsolatedDealloc) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s4test37ProbeDefaultAsync_BaseIsolatedDeallocCfZ'

// CHECK-SYMB: // ProbeDefaultAsync_BaseIsolatedDealloc.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test37ProbeDefaultAsync_BaseIsolatedDeallocCfD : $@convention(method) (@owned ProbeDefaultAsync_BaseIsolatedDealloc) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s4test37ProbeDefaultAsync_BaseIsolatedDeallocCfD'
class ProbeDefaultAsync_BaseIsolatedDealloc: BaseIsolatedDealloc {
    // expected-warning@+1 {{async deinit contains no await statements; consider using isolated sync deinit instead}}
    deinit async {
        isolatedFunc()
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeNonisolatedAsync_BaseIsolatedDealloc : BaseIsolatedDealloc {
// CHECK: {{(@objc )?}} nonisolated deinit async
// CHECK: }

// CHECK-SYMB: // ProbeNonisolatedAsync_BaseIsolatedDealloc.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test41ProbeNonisolatedAsync_BaseIsolatedDeallocCfZ : $@convention(thin) @async (@owned ProbeNonisolatedAsync_BaseIsolatedDealloc) -> () {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-SYMB-NEXT: hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s4test12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NEXT: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = objc_super_method %0 : $ProbeNonisolatedAsync_BaseIsolatedDealloc, #BaseIsolatedDealloc.deinit!deallocator.foreign : (BaseIsolatedDealloc) -> () -> (), $@convention(objc_method) (BaseIsolatedDealloc) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(objc_method) (BaseIsolatedDealloc) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s4test41ProbeNonisolatedAsync_BaseIsolatedDeallocCfZ'

// CHECK-SYMB: // ProbeNonisolatedAsync_BaseIsolatedDealloc.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test41ProbeNonisolatedAsync_BaseIsolatedDeallocCfD : $@convention(method) (@owned ProbeNonisolatedAsync_BaseIsolatedDealloc) -> () {
// CHECK-SYMB: [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[GENERIC_EXEC]]
// CHECK-SYMB: } // end sil function '$s4test41ProbeNonisolatedAsync_BaseIsolatedDeallocCfD'
class ProbeNonisolatedAsync_BaseIsolatedDealloc: BaseIsolatedDealloc {
    nonisolated deinit async {
        await isolatedFunc()
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeGlobalAsync_Intermediate_BaseIsolatedDealloc : Intermediate {
// CHECK: {{(@objc )?}} @AnotherActor deinit async
// CHECK: }

// CHECK-SYMB: // ProbeGlobalAsync_Intermediate_BaseIsolatedDealloc.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test49ProbeGlobalAsync_Intermediate_BaseIsolatedDeallocCfZ : $@convention(thin) @async (@owned ProbeGlobalAsync_Intermediate_BaseIsolatedDealloc) -> () {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s4test12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NEXT: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = function_ref @$s4test12IntermediateCfZ : $@convention(thin) @async (@owned Intermediate) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(thin) @async (@owned Intermediate) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s4test49ProbeGlobalAsync_Intermediate_BaseIsolatedDeallocCfZ'

// CHECK-SYMB: // ProbeGlobalAsync_Intermediate_BaseIsolatedDealloc.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test49ProbeGlobalAsync_Intermediate_BaseIsolatedDeallocCfD : $@convention(method) (@owned ProbeGlobalAsync_Intermediate_BaseIsolatedDealloc) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s4test49ProbeGlobalAsync_Intermediate_BaseIsolatedDeallocCfD'
class Intermediate: BaseIsolatedDealloc {
    deinit async {
        await Task.yield()
    }
}
class ProbeGlobalAsync_Intermediate_BaseIsolatedDealloc: Intermediate {
    @AnotherActor deinit async {
#if SILGEN
        await isolatedFunc()
#else
        // expected-error@+2 {{expression is 'async' but is not marked with 'await'}}
        // expected-note@+1 {{calls to global function 'isolatedFunc()' from outside of its actor context are implicitly asynchronous}}
        isolatedFunc()
#endif
    }
}

// MARK: - DerivedIsolatedDealloc

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_DerivedIsolatedDealloc : DerivedIsolatedDealloc {
// Note: Type-checked as isolated, but no @MainActor attribute, because attributes are not added for overriding members
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_DerivedIsolatedDealloc.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test36ProbeImplicit_DerivedIsolatedDeallocCfZ
// CHECK-SYMB: // ProbeImplicit_DerivedIsolatedDealloc.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test36ProbeImplicit_DerivedIsolatedDeallocCfD : $@convention(method) (@owned ProbeImplicit_DerivedIsolatedDealloc) -> () {
class ProbeImplicit_DerivedIsolatedDealloc: DerivedIsolatedDealloc {}

#if !SILGEN
class ProbeDefault_DerivedIsolatedDealloc: DerivedIsolatedDealloc {
    deinit { // expected-error {{nonisolated deinitializer 'deinit' has different actor isolation from main actor-isolated overridden declaration}}
        isolatedFunc() // expected-error {{call to main actor-isolated global function 'isolatedFunc()' in a synchronous nonisolated context}}
    }
}
#endif

#if !SILGEN
class ProbeIsolated_DerivedIsolatedDealloc: DerivedIsolatedDealloc {
    isolated deinit { //expected-error {{deinit is marked isolated, but containing class 'ProbeIsolated_DerivedIsolatedDealloc' is not isolated to an actor}}
        isolatedFunc() // ok, isolation of the overridden deinit is used as a recovery strategy
    }
}
#endif

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeGlobal_DerivedIsolatedDealloc : DerivedIsolatedDealloc {
// CHECK: @objc @MainActor deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeGlobal_DerivedIsolatedDealloc.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test34ProbeGlobal_DerivedIsolatedDeallocCfZ
// CHECK-SYMB: // ProbeGlobal_DerivedIsolatedDealloc.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test34ProbeGlobal_DerivedIsolatedDeallocCfD : $@convention(method) (@owned ProbeGlobal_DerivedIsolatedDealloc) -> () {
class ProbeGlobal_DerivedIsolatedDealloc: DerivedIsolatedDealloc {
#if !SILGEN
    @AnotherActor deinit {} // expected-error {{global actor 'AnotherActor'-isolated deinitializer 'deinit' has different actor isolation from main actor-isolated overridden declaration}}
#else
    @MainActor deinit {}
#endif
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeDefaultAsync_DerivedIsolatedDealloc : DerivedIsolatedDealloc {
// CHECK: {{(@objc )?}} deinit async
// CHECK: }

// CHECK-SYMB: // ProbeDefaultAsync_DerivedIsolatedDealloc.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test40ProbeDefaultAsync_DerivedIsolatedDeallocCfZ : $@convention(thin) @async (@owned ProbeDefaultAsync_DerivedIsolatedDealloc) -> () {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s4test12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = objc_super_method %0 : $ProbeDefaultAsync_DerivedIsolatedDealloc, #DerivedIsolatedDealloc.deinit!deallocator.foreign : (DerivedIsolatedDealloc) -> () -> (), $@convention(objc_method) (DerivedIsolatedDealloc) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(objc_method) (DerivedIsolatedDealloc) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s4test40ProbeDefaultAsync_DerivedIsolatedDeallocCfZ'

// CHECK-SYMB: // ProbeDefaultAsync_DerivedIsolatedDealloc.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test40ProbeDefaultAsync_DerivedIsolatedDeallocCfD : $@convention(method) (@owned ProbeDefaultAsync_DerivedIsolatedDealloc) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s4test40ProbeDefaultAsync_DerivedIsolatedDeallocCfD'
class ProbeDefaultAsync_DerivedIsolatedDealloc: DerivedIsolatedDealloc {
    // expected-warning@+1 {{async deinit contains no await statements; consider using isolated sync deinit instead}}
    deinit async {
        isolatedFunc()
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeNonisolatedAsync_DerivedIsolatedDealloc : DerivedIsolatedDealloc {
// CHECK: {{(@objc )?}} nonisolated deinit async
// CHECK: }

// CHECK-SYMB: // ProbeNonisolatedAsync_DerivedIsolatedDealloc.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test44ProbeNonisolatedAsync_DerivedIsolatedDeallocCfZ : $@convention(thin) @async (@owned ProbeNonisolatedAsync_DerivedIsolatedDealloc) -> () {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-SYMB-NEXT: hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s4test12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NEXT: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor [[GENERIC_EXEC]] : $Optional<Builtin.Executor>
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = objc_super_method %0 : $ProbeNonisolatedAsync_DerivedIsolatedDealloc, #DerivedIsolatedDealloc.deinit!deallocator.foreign : (DerivedIsolatedDealloc) -> () -> (), $@convention(objc_method) (DerivedIsolatedDealloc) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(objc_method) (DerivedIsolatedDealloc) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s4test44ProbeNonisolatedAsync_DerivedIsolatedDeallocCfZ'

// CHECK-SYMB: // ProbeNonisolatedAsync_DerivedIsolatedDealloc.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test44ProbeNonisolatedAsync_DerivedIsolatedDeallocCfD : $@convention(method) (@owned ProbeNonisolatedAsync_DerivedIsolatedDealloc) -> () {
// CHECK-SYMB: [[GENERIC_EXEC:%.*]] = enum $Optional<Builtin.Executor>, #Optional.none
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[GENERIC_EXEC]]
// CHECK-SYMB: } // end sil function '$s4test44ProbeNonisolatedAsync_DerivedIsolatedDeallocCfD'
class ProbeNonisolatedAsync_DerivedIsolatedDealloc: DerivedIsolatedDealloc {
    nonisolated deinit async {
        await isolatedFunc()
    }
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeGlobalAsync_DerivedIsolatedDealloc : DerivedIsolatedDealloc {
// CHECK: {{(@objc )?}} @AnotherActor deinit async
// CHECK: }

// CHECK-SYMB: // ProbeGlobalAsync_DerivedIsolatedDealloc.__isolated_deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test39ProbeGlobalAsync_DerivedIsolatedDeallocCfZ : $@convention(thin) @async (@owned ProbeGlobalAsync_DerivedIsolatedDealloc) -> () {
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[FUNC:%.*]] = function_ref @$s4test12isolatedFuncyyF : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NEXT: apply [[FUNC]]() : $@convention(thin) () -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: hop_to_executor {{%[0-9]+}} : $MainActor
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: [[SUPER_DEINIT:%.*]] = objc_super_method %0 : $ProbeGlobalAsync_DerivedIsolatedDealloc, #DerivedIsolatedDealloc.deinit!deallocator.foreign : (DerivedIsolatedDealloc) -> () -> (), $@convention(objc_method) (DerivedIsolatedDealloc) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: apply [[SUPER_DEINIT]]({{%[0-9]+}}) : $@convention(objc_method) (DerivedIsolatedDealloc) -> ()
// CHECK-SYMB-NOT: hop_to_executor
// CHECK-SYMB: } // end sil function '$s4test39ProbeGlobalAsync_DerivedIsolatedDeallocCfZ'

// CHECK-SYMB: // ProbeGlobalAsync_DerivedIsolatedDealloc.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test39ProbeGlobalAsync_DerivedIsolatedDeallocCfD : $@convention(method) (@owned ProbeGlobalAsync_DerivedIsolatedDealloc) -> () {
// CHECK-SYMB: [[EXECUTOR:%.*]] = extract_executor {{%[0-9]+}} : $AnotherActor
// CHECK-SYMB: [[OPT_EXECUTOR:%.*]] = enum $Optional<Builtin.Executor>, #Optional.some!enumelt, [[EXECUTOR]] : $Builtin.Executor
// CHECK-SYMB: [[DEINIT_ASYNC:%.*]] = function_ref @swift_task_deinitAsync : $@convention(thin) (@owned AnyObject, @convention(thin) @async (@owned AnyObject) -> (), Optional<Builtin.Executor>, Builtin.Word) -> ()
// CHECK-SYMB: apply [[DEINIT_ASYNC]]({{%[0-9]+, %[0-9]+, }}[[OPT_EXECUTOR]]
// CHECK-SYMB: } // end sil function '$s4test39ProbeGlobalAsync_DerivedIsolatedDeallocCfD'
class ProbeGlobalAsync_DerivedIsolatedDealloc: DerivedIsolatedDealloc {
    @AnotherActor deinit async {
#if SILGEN
        await isolatedFunc()
#else
        // expected-error@+2 {{expression is 'async' but is not marked with 'await'}}
        // expected-note@+1 {{calls to global function 'isolatedFunc()' from outside of its actor context are implicitly asynchronous}}
        isolatedFunc()
#endif
    }
}

// MARK: - Isolated dealloc outside main declaration

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_DeallocIsolatedFromProtocol : DeallocIsolatedFromProtocol {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_DeallocIsolatedFromProtocol.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test41ProbeImplicit_DeallocIsolatedFromProtocolCfZ
// CHECK-SYMB: // ProbeImplicit_DeallocIsolatedFromProtocol.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test41ProbeImplicit_DeallocIsolatedFromProtocolCfD : $@convention(method) (@owned ProbeImplicit_DeallocIsolatedFromProtocol) -> () {
class ProbeImplicit_DeallocIsolatedFromProtocol: DeallocIsolatedFromProtocol {}

class ProbeGlobal_DeallocIsolatedFromProtocol: DeallocIsolatedFromProtocol {
    @AnotherActor deinit {} // ok, base is not isolated
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_DeallocIsolatedFromCategory : DeallocIsolatedFromCategory {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_DeallocIsolatedFromCategory.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test41ProbeImplicit_DeallocIsolatedFromCategoryCfZ
// CHECK-SYMB: // ProbeImplicit_DeallocIsolatedFromCategory.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test41ProbeImplicit_DeallocIsolatedFromCategoryCfD : $@convention(method) (@owned ProbeImplicit_DeallocIsolatedFromCategory) -> () {
class ProbeImplicit_DeallocIsolatedFromCategory: DeallocIsolatedFromCategory {}

class ProbeGlobal_DeallocIsolatedFromCategory: DeallocIsolatedFromCategory {
    @AnotherActor deinit {} // ok, base is not isolated
}

// CHECK-LABEL: @objc @_inheritsConvenienceInitializers class ProbeImplicit_DeallocIsolatedFromExtension : DeallocIsolatedFromExtension {
// CHECK: @objc deinit
// CHECK: }
// CHECK-SYMB-NOT: ProbeImplicit_DeallocIsolatedFromExtension.__isolated_deallocating_deinit
// CHECK-SYMB-NOT: @$s4test42ProbeImplicit_DeallocIsolatedFromExtensionCfZ
// CHECK-SYMB: // ProbeImplicit_DeallocIsolatedFromExtension.__deallocating_deinit
// CHECK-SYMB-NEXT: sil hidden [ossa] @$s4test42ProbeImplicit_DeallocIsolatedFromExtensionCfD : $@convention(method) (@owned ProbeImplicit_DeallocIsolatedFromExtension) -> () {
class ProbeImplicit_DeallocIsolatedFromExtension: DeallocIsolatedFromExtension {}

class ProbeGlobal_DeallocIsolatedFromExtension: DeallocIsolatedFromExtension {
    @AnotherActor deinit {} // ok, base is not isolated
}
