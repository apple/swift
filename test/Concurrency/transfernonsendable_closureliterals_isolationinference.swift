// RUN: %target-swift-frontend -emit-sil -parse-as-library -disable-availability-checking -swift-version 5 -strict-concurrency=complete %s -o - | %FileCheck %s
// RUN: %target-swift-frontend -emit-sil -parse-as-library -disable-availability-checking -swift-version 6 -verify %s -o /dev/null

// REQUIRES: concurrency
// REQUIRES: asserts

// This test validates the behavior of transfernonsendable around
// closure literals

////////////////////////
// MARK: Declarations //
////////////////////////

class NonSendableKlass {}

actor CustomActorInstance {}

@globalActor
struct CustomActor {
  static let shared = CustomActorInstance()
}

func normalAcceptsClosure(_ x: () -> ()) {}
func normalAcceptsSendingClosure(_ x: sending () -> ()) {}
func normalAcceptsSendableClosure(_ x: @Sendable () -> ()) {}

func inheritActorContextAcceptsClosure(@_inheritActorContext _ x: () -> ()) { }
func inheritActorContextAcceptsSendingClosure(@_inheritActorContext _ x: sending () -> ()) { }
func inheritActorContextAcceptsSendableClosure(@_inheritActorContext _ x: @Sendable () -> ()) { }

@MainActor
func normalGlobalActorAcceptsClosure(_ x: () -> ()) { }
@MainActor
func normalGlobalActorAcceptsSendingClosure(_ x: sending () -> ()) { }
@MainActor
func normalGlobalActorAcceptsSendableClosure(_ x: @Sendable () -> ()) { }

@MainActor
func inheritActorContextGlobalActorAcceptsClosure(@_inheritActorContext _ x: () -> ()) { }
@MainActor
func inheritActorContextGlobalActorAcceptsSendingClosure(@_inheritActorContext _ x: sending () -> ()) { }
@MainActor
func inheritActorContextGlobalActorAcceptsSendableClosure(@_inheritActorContext _ x: @Sendable () -> ()) { }

func normalAcceptsAsyncClosure(_ x: () async -> ()) {}
func normalAcceptsSendingAsyncClosure(_ x: sending () async -> ()) {}
func normalAcceptsSendableAsyncClosure(_ x: @Sendable () async -> ()) {}

func inheritActorContextAcceptsAsyncClosure(@_inheritActorContext _ x: () async -> ()) { }
func inheritActorContextAcceptsSendingAsyncClosure(@_inheritActorContext _ x: sending () async -> ()) { }
func inheritActorContextAcceptsSendableAsyncClosure(@_inheritActorContext _ x: @Sendable () async -> ()) { }

@MainActor
func normalGlobalActorAcceptsAsyncClosure(_ x: () async -> ()) { }
@MainActor
func normalGlobalActorAcceptsSendingAsyncClosure(_ x: sending () async -> ()) { }
@MainActor
func normalGlobalActorAcceptsSendableAsyncClosure(_ x: @Sendable () async -> ()) { }

@MainActor
func inheritActorContextGlobalActorAcceptsAsyncClosure(@_inheritActorContext _ x: () async -> ()) { }
@MainActor
func inheritActorContextGlobalActorAcceptsSendingAsyncClosure(@_inheritActorContext _ x: sending () async -> ()) { }
@MainActor
func inheritActorContextGlobalActorAcceptsSendableAsyncClosure(@_inheritActorContext _ x: @Sendable () async -> ()) { }

func asyncNormalAcceptsClosure(_ x: () -> ()) async {}
func asyncNormalAcceptsSendingClosure(_ x: sending () -> ()) async {}
func asyncNormalAcceptsSendableClosure(_ x: @Sendable () -> ()) async {}

func asyncInheritActorContextAcceptsClosure(@_inheritActorContext _ x: () -> ()) async {}
func asyncInheritActorContextAcceptsSendingClosure(@_inheritActorContext _ x: sending () -> ()) async {}
func asyncInheritActorContextAcceptsSendableClosure(@_inheritActorContext _ x: @Sendable () -> ()) async {}

@MainActor
func asyncNormalGlobalActorAcceptsClosure(_ x: () -> ()) async {}
@MainActor
func asyncNormalGlobalActorAcceptsSendingClosure(_ x: sending () -> ()) async {}
@MainActor
func asyncNormalGlobalActorAcceptsSendableClosure(_ x: @Sendable () -> ()) async {}

@MainActor
func asyncInheritActorContextGlobalActorAcceptsClosure(@_inheritActorContext _ x: () -> ()) async {}
@MainActor
func asyncInheritActorContextGlobalActorAcceptsSendingClosure(@_inheritActorContext _ x: sending () -> ()) async {}
@MainActor
func asyncInheritActorContextGlobalActorAcceptsSendableClosure(@_inheritActorContext _ x: @Sendable () -> ()) async {}

func asyncNormalAcceptsAsyncClosure(_ x: () async -> ()) async {}
func asyncNormalAcceptsSendingAsyncClosure(_ x: sending () async -> ()) async {}
func asyncNormalAcceptsSendableAsyncClosure(_ x: @Sendable () async -> ()) async {}

func asyncInheritActorContextAcceptsAsyncClosure(@_inheritActorContext _ x: () async -> ()) async {}
func asyncInheritActorContextAcceptsSendingAsyncClosure(@_inheritActorContext _ x: sending () async -> ()) async {}
func asyncInheritActorContextAcceptsSendableAsyncClosure(@_inheritActorContext _ x: @Sendable () async -> ()) async {}

@MainActor
func asyncNormalGlobalActorAcceptsAsyncClosure(_ x: () async -> ()) async {}
@MainActor
func asyncNormalGlobalActorAcceptsSendingAsyncClosure(_ x: sending () async -> ()) async {}
@MainActor
func asyncNormalGlobalActorAcceptsSendableAsyncClosure(_ x: @Sendable () async -> ()) async {}

@MainActor
func asyncInheritActorContextGlobalActorAcceptsAsyncClosure(@_inheritActorContext _ x: () async -> ()) async {}
@MainActor
func asyncInheritActorContextGlobalActorAcceptsSendingAsyncClosure(@_inheritActorContext _ x: sending () async -> ()) async {}
@MainActor
func asyncInheritActorContextGlobalActorAcceptsSendableAsyncClosure(@_inheritActorContext _ x: @Sendable () async -> ()) async {}

actor MyActor {}

@MainActor
var mainActorIsolatedValue = NonSendableKlass()

func useValue<T>(_ x: T) { }

//////////////////////////////////////////////////////
// MARK: Global Actor: Sync User Sync Closure Tests //
//////////////////////////////////////////////////////

@CustomActor
func test_CallerSyncNormal_CalleeSyncNonIsolated() async {
    // Just to help start our pattern matchin since the closure #$NUM line
    // appears in other parts of the file.
    // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference028test_CallerSyncNormal_CalleeF11NonIsolatedyyYaF'

    // CHECK-LABEL: closure #1 in test_CallerSyncNormal_CalleeSyncNonIsolated()
    // CHECK-NEXT: Isolation: global_actor. type: CustomActor
    normalAcceptsClosure { }

    // CHECK-LABEL: closure #2 in test_CallerSyncNormal_CalleeSyncNonIsolated()
    // CHECK-NEXT: Isolation: nonisolated
    normalAcceptsSendingClosure { }

    // CHECK-LABEL: // closure #3 in test_CallerSyncNormal_CalleeSyncNonIsolated()
    // CHECK-NEXT: // Isolation: nonisolated
    normalAcceptsSendableClosure { }
}

@CustomActor
func test_CallerSyncInheritsActorContext_CalleeSyncNonisolated() {
    // CHECK: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference042test_CallerSyncInheritsActorContext_CalleeF11NonisolatedyyF'

    // CHECK-LABEL: // closure #1 in test_CallerSyncInheritsActorContext_CalleeSyncNonisolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    inheritActorContextAcceptsClosure { }

    // This is a synchronous closure, so we error here.
    //
    // CHECK-LABEL: // closure #2 in test_CallerSyncInheritsActorContext_CalleeSyncNonisolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    inheritActorContextAcceptsSendingClosure { } // expected-error {{global actor 'CustomActor'-isolated value of type '() -> ()' passed as a strongly transferred parameter}}

    // CHECK-LABEL: // closure #3 in test_CallerSyncInheritsActorContext_CalleeSyncNonisolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    inheritActorContextAcceptsSendableClosure { }
}

@CustomActor
func test_CallerSyncNormal_CalleeSyncMainActorIsolated() async {
    // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference028test_CallerSyncNormal_CalleeF17MainActorIsolatedyyYaF'

    // CHECK-LABEL: // closure #1 in test_CallerSyncNormal_CalleeSyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await normalGlobalActorAcceptsClosure { } // expected-error {{sending global actor 'CustomActor'-isolated value of type '() -> ()' with later accesses to main actor-isolated context risks causing data races}}

    // CHECK-LABEL: // closure #2 in test_CallerSyncNormal_CalleeSyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: nonisolated
    await normalGlobalActorAcceptsSendingClosure { }

    // CHECK-LABEL: // closure #3 in test_CallerSyncNormal_CalleeSyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: nonisolated
    await normalGlobalActorAcceptsSendableClosure { }
}

@CustomActor
func test_CallerSyncInheritsActorContext_CalleeSyncMainActorIsolated() async {
    // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference042test_CallerSyncInheritsActorContext_Calleef4MainH8IsolatedyyYaF'

    // CHECK-LABEL: // closure #1 in test_CallerSyncInheritsActorContext_CalleeSyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await inheritActorContextGlobalActorAcceptsClosure { } // expected-error {{sending global actor 'CustomActor'-isolated value of type '() -> ()' with later accesses to main actor-isolated context risks causing data races}}

    // This is a synchronous closure, so we error here.
    //
    // CHECK-LABEL: // closure #2 in test_CallerSyncInheritsActorContext_CalleeSyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await inheritActorContextGlobalActorAcceptsSendingClosure { } // expected-error {{global actor 'CustomActor'-isolated value of type '() -> ()' passed as a strongly transferred parameter}}

    // CHECK-LABEL: // closure #3 in test_CallerSyncInheritsActorContext_CalleeSyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await inheritActorContextGlobalActorAcceptsSendableClosure { }
}

////////////////////////////////////////////////
// MARK: Global Actor Sync User Async Closure //
////////////////////////////////////////////////

@CustomActor
func test_CallerSyncNormal_CalleeAsyncNonIsolated() async {
    // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference44test_CallerSyncNormal_CalleeAsyncNonIsolatedyyYaF'
    // CHECK-LABEL: closure #1 in test_CallerSyncNormal_CalleeAsyncNonIsolated()
    // CHECK-NEXT: Isolation: global_actor. type: CustomActor
    normalAcceptsAsyncClosure { }

    // CHECK-LABEL: closure #2 in test_CallerSyncNormal_CalleeAsyncNonIsolated()
    // CHECK-NEXT: Isolation: nonisolated
    normalAcceptsSendingAsyncClosure { }

    // CHECK-LABEL: // closure #3 in test_CallerSyncNormal_CalleeAsyncNonIsolated()
    // CHECK-NEXT: // Isolation: nonisolated
    normalAcceptsSendableAsyncClosure { }
}

@CustomActor
func test_CallerSyncInheritsActorContext_CalleeAsyncNonisolated() {
    // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference58test_CallerSyncInheritsActorContext_CalleeAsyncNonisolatedyyF'

    // CHECK-LABEL: // closure #1 in test_CallerSyncInheritsActorContext_CalleeAsyncNonisolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    inheritActorContextAcceptsAsyncClosure { }

    // CHECK-LABEL: // closure #2 in test_CallerSyncInheritsActorContext_CalleeAsyncNonisolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    inheritActorContextAcceptsSendingAsyncClosure { }

    // CHECK-LABEL: // closure #3 in test_CallerSyncInheritsActorContext_CalleeAsyncNonisolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    inheritActorContextAcceptsSendableAsyncClosure { }
}

@CustomActor
func test_CallerSyncNormal_CalleeAsyncMainActorIsolated() async {
    // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference50test_CallerSyncNormal_CalleeAsyncMainActorIsolatedyyYaF'

    // CHECK-LABEL: // closure #1 in test_CallerSyncNormal_CalleeAsyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await normalGlobalActorAcceptsAsyncClosure { } // expected-error {{sending global actor 'CustomActor'-isolated value of type '() async -> ()' with later accesses to main actor-isolated context risks causing data races}}

    // CHECK-LABEL: // closure #2 in test_CallerSyncNormal_CalleeAsyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: nonisolated
    await normalGlobalActorAcceptsSendingAsyncClosure { }

    // CHECK-LABEL: // closure #3 in test_CallerSyncNormal_CalleeAsyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: nonisolated
    await normalGlobalActorAcceptsSendableAsyncClosure { }
}

@CustomActor
func test_CallerSyncInheritsActorContext_CalleeAsyncMainActorIsolated() async {
    // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference051test_CallerSyncInheritsActorContext_CalleeAsyncMainH8IsolatedyyYaF'

    // CHECK-LABEL: // closure #1 in test_CallerSyncInheritsActorContext_CalleeAsyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await inheritActorContextGlobalActorAcceptsAsyncClosure { }

    // CHECK-LABEL: // closure #2 in test_CallerSyncInheritsActorContext_CalleeAsyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await inheritActorContextGlobalActorAcceptsSendingAsyncClosure { }

    // CHECK-LABEL: // closure #3 in test_CallerSyncInheritsActorContext_CalleeAsyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await inheritActorContextGlobalActorAcceptsSendableAsyncClosure { }
}

//////////////////////////////////////////////////////
// MARK: Global Actor Async User Sync Closure Tests //
//////////////////////////////////////////////////////

@CustomActor
func test_CallerAsyncNormal_CalleeSyncNonIsolated() async {
    // Just to help start our pattern matchin since the closure #$NUM line
    // appears in other parts of the file.
    // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference44test_CallerAsyncNormal_CalleeSyncNonIsolatedyyYaF'

    // CHECK-LABEL: closure #1 in test_CallerAsyncNormal_CalleeSyncNonIsolated()
    // CHECK-NEXT: Isolation: global_actor. type: CustomActor
    await asyncNormalAcceptsClosure { } // expected-error {{sending global actor 'CustomActor'-isolated value of type '() -> ()' with later accesses to nonisolated context risks causing data races}}

    // CHECK-LABEL: closure #2 in test_CallerAsyncNormal_CalleeSyncNonIsolated()
    // CHECK-NEXT: Isolation: nonisolated
    await asyncNormalAcceptsSendingClosure { }

    // CHECK-LABEL: // closure #3 in test_CallerAsyncNormal_CalleeSyncNonIsolated()
    // CHECK-NEXT: // Isolation: nonisolated
    await asyncNormalAcceptsSendableClosure { }
}

@CustomActor
func test_CallerAsyncInheritsActorContext_CalleeSyncNonisolated() async {
    // CHECK: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference58test_CallerAsyncInheritsActorContext_CalleeSyncNonisolatedyyYaF'

    // CHECK-LABEL: // closure #1 in test_CallerAsyncInheritsActorContext_CalleeSyncNonisolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await asyncInheritActorContextAcceptsClosure { } // expected-error {{sending global actor 'CustomActor'-isolated value of type '() -> ()' with later accesses to nonisolated context risks causing data races}}

    // This is a synchronous closure, so we error here.
    //
    // CHECK-LABEL: // closure #2 in test_CallerAsyncInheritsActorContext_CalleeSyncNonisolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await asyncInheritActorContextAcceptsSendingClosure { } // expected-error {{global actor 'CustomActor'-isolated value of type '() -> ()' passed as a strongly transferred parameter}}

    // CHECK-LABEL: // closure #3 in test_CallerAsyncInheritsActorContext_CalleeSyncNonisolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await asyncInheritActorContextAcceptsSendableClosure { }
}

@CustomActor
func test_CallerAsyncNormal_CalleeSyncMainActorIsolated() async {
    // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference50test_CallerAsyncNormal_CalleeSyncMainActorIsolatedyyYaF'

    // CHECK-LABEL: // closure #1 in test_CallerAsyncNormal_CalleeSyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await asyncNormalGlobalActorAcceptsClosure { } // expected-error {{sending global actor 'CustomActor'-isolated value of type '() -> ()' with later accesses to main actor-isolated context risks causing data races}}

    // CHECK-LABEL: // closure #2 in test_CallerAsyncNormal_CalleeSyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: nonisolated
    await asyncNormalGlobalActorAcceptsSendingClosure { }

    // CHECK-LABEL: // closure #3 in test_CallerAsyncNormal_CalleeSyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: nonisolated
    await asyncNormalGlobalActorAcceptsSendableClosure { }
}

@CustomActor
func test_CallerAsyncInheritsActorContext_CalleeSyncMainActorIsolated() async {
    // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference051test_CallerAsyncInheritsActorContext_CalleeSyncMainH8IsolatedyyYaF'

    // CHECK-LABEL: // closure #1 in test_CallerAsyncInheritsActorContext_CalleeSyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await asyncInheritActorContextGlobalActorAcceptsClosure { } // expected-error {{sending global actor 'CustomActor'-isolated value of type '() -> ()' with later accesses to main actor-isolated context risks causing data races}}

    // This is a synchronous closure, so we error here.
    //
    // CHECK-LABEL: // closure #2 in test_CallerAsyncInheritsActorContext_CalleeSyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await asyncInheritActorContextGlobalActorAcceptsSendingClosure { } // expected-error {{global actor 'CustomActor'-isolated value of type '() -> ()' passed as a strongly transferred parameter}}

    // CHECK-LABEL: // closure #3 in test_CallerAsyncInheritsActorContext_CalleeSyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await asyncInheritActorContextGlobalActorAcceptsSendableClosure { }
}

/////////////////////////////////////////////////
// MARK: Global Actor Async User Async Closure //
/////////////////////////////////////////////////

@CustomActor
func test_CallerAsyncNormal_CalleeAsyncNonIsolated() async {
    // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference029test_CallerAsyncNormal_CalleeF11NonIsolatedyyYaF'

    // CHECK-LABEL: closure #1 in test_CallerAsyncNormal_CalleeAsyncNonIsolated()
    // CHECK-NEXT: Isolation: global_actor. type: CustomActor
    await asyncNormalAcceptsAsyncClosure { } // expected-error {{sending global actor 'CustomActor'-isolated value of type '() async -> ()' with later accesses to nonisolated context risks causing data races}}

    // CHECK-LABEL: closure #2 in test_CallerAsyncNormal_CalleeAsyncNonIsolated()
    // CHECK-NEXT: Isolation: nonisolated
    await asyncNormalAcceptsSendingAsyncClosure { }

    // CHECK-LABEL: // closure #3 in test_CallerAsyncNormal_CalleeAsyncNonIsolated()
    // CHECK-NEXT: // Isolation: nonisolated
    await asyncNormalAcceptsSendableAsyncClosure { }
}

@CustomActor
func test_CallerAsyncInheritsActorContext_CalleeAsyncNonisolated() async {
    // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference043test_CallerAsyncInheritsActorContext_CalleeF11NonisolatedyyYaF'

    // CHECK-LABEL: // closure #1 in test_CallerAsyncInheritsActorContext_CalleeAsyncNonisolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await asyncInheritActorContextAcceptsAsyncClosure { }

    // CHECK-LABEL: // closure #2 in test_CallerAsyncInheritsActorContext_CalleeAsyncNonisolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await asyncInheritActorContextAcceptsSendingAsyncClosure { }

    // CHECK-LABEL: // closure #3 in test_CallerAsyncInheritsActorContext_CalleeAsyncNonisolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await asyncInheritActorContextAcceptsSendingAsyncClosure { @CustomActor in }

    // CHECK-LABEL: // closure #4 in test_CallerAsyncInheritsActorContext_CalleeAsyncNonisolated()
    // CHECK-NEXT: // Isolation: global_actor. type: MainActor
    await asyncInheritActorContextAcceptsSendingAsyncClosure { @MainActor in }

    // CHECK-LABEL: // closure #5 in test_CallerAsyncInheritsActorContext_CalleeAsyncNonisolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await asyncInheritActorContextAcceptsSendableAsyncClosure { }
}

@CustomActor
func test_CallerAsyncNormal_CalleeAsyncMainActorIsolated() async {
    // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference029test_CallerAsyncNormal_CalleeF17MainActorIsolatedyyYaF'

    // CHECK-LABEL: // closure #1 in test_CallerAsyncNormal_CalleeAsyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await asyncNormalGlobalActorAcceptsAsyncClosure { } // expected-error {{sending global actor 'CustomActor'-isolated value of type '() async -> ()' with later accesses to main actor-isolated context risks causing data races}}

    // CHECK-LABEL: // closure #2 in test_CallerAsyncNormal_CalleeAsyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: nonisolated
    await asyncNormalGlobalActorAcceptsSendingAsyncClosure { }

    // CHECK-LABEL: // closure #3 in test_CallerAsyncNormal_CalleeAsyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: nonisolated
    await asyncNormalGlobalActorAcceptsSendableAsyncClosure { }
}

@CustomActor
func test_CallerAsyncInheritsActorContext_CalleeAsyncMainActorIsolated() async {
    // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference043test_CallerAsyncInheritsActorContext_Calleef4MainH8IsolatedyyYaF'

    // CHECK-LABEL: // closure #1 in test_CallerAsyncInheritsActorContext_CalleeAsyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await asyncInheritActorContextGlobalActorAcceptsAsyncClosure { }

    // CHECK-LABEL: // closure #2 in test_CallerAsyncInheritsActorContext_CalleeAsyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await asyncInheritActorContextGlobalActorAcceptsSendingAsyncClosure { }

    // CHECK-LABEL: // closure #3 in test_CallerAsyncInheritsActorContext_CalleeAsyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await asyncInheritActorContextGlobalActorAcceptsSendingAsyncClosure { @CustomActor in }

    // CHECK-LABEL: // closure #4 in test_CallerAsyncInheritsActorContext_CalleeAsyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: MainActor
    await asyncInheritActorContextGlobalActorAcceptsSendingAsyncClosure { @MainActor in }

    // CHECK-LABEL: // closure #5 in test_CallerAsyncInheritsActorContext_CalleeAsyncMainActorIsolated()
    // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
    await asyncInheritActorContextGlobalActorAcceptsSendableAsyncClosure { }
}

////////////////////////////////////////////////////////
// MARK: Actor Instance: Sync User Sync Closure Tests //
////////////////////////////////////////////////////////

extension MyActor {
    func test_CallerSyncNormal_CalleeSyncNonIsolated() async {
        // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference7MyActorC028test_CallerSyncNormal_CalleeH11NonIsolatedyyYaF'

        // CHECK-LABEL: closure #1 in MyActor.test_CallerSyncNormal_CalleeSyncNonIsolated()
        // CHECK-NEXT: Isolation: actor_instance. name: 'self'
        normalAcceptsClosure { print(self) }

        // CHECK-LABEL: closure #2 in MyActor.test_CallerSyncNormal_CalleeSyncNonIsolated()
        // CHECK-NEXT: Isolation: nonisolated
        normalAcceptsSendingClosure { print(self) }

        // CHECK-LABEL: // closure #3 in MyActor.test_CallerSyncNormal_CalleeSyncNonIsolated()
        // CHECK-NEXT: // Isolation: nonisolated
        normalAcceptsSendableClosure { print(self) }
    }

    func test_CallerSyncInheritsActorContext_CalleeSyncNonisolated() {
        // CHECK: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference7MyActorC023test_CallerSyncInheritse14Context_CalleeH11NonisolatedyyF'

        // CHECK-LABEL: // closure #1 in MyActor.test_CallerSyncInheritsActorContext_CalleeSyncNonisolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        inheritActorContextAcceptsClosure { print(self) }

        // This is a synchronous closure, so we error here.
        //
        // CHECK-LABEL: // closure #2 in MyActor.test_CallerSyncInheritsActorContext_CalleeSyncNonisolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        inheritActorContextAcceptsSendingClosure { print(self) } // expected-error {{'self'-isolated value of type '() -> ()' passed as a strongly transferred parameter}}

        // CHECK-LABEL: // closure #3 in MyActor.test_CallerSyncInheritsActorContext_CalleeSyncNonisolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        inheritActorContextAcceptsSendableClosure { print(self) }
    }

    func test_CallerSyncNormal_CalleeSyncMainActorIsolated() async {
        // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference7MyActorC028test_CallerSyncNormal_Calleeh4MainE8IsolatedyyYaF'

        // CHECK-LABEL: // closure #1 in MyActor.test_CallerSyncNormal_CalleeSyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await normalGlobalActorAcceptsClosure { print(self) } // expected-error {{sending 'self'-isolated value of type '() -> ()' with later accesses to main actor-isolated context risks causing data races}}

        // CHECK-LABEL: // closure #2 in MyActor.test_CallerSyncNormal_CalleeSyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: nonisolated
        await normalGlobalActorAcceptsSendingClosure { print(self) }

        // CHECK-LABEL: // closure #3 in MyActor.test_CallerSyncNormal_CalleeSyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: nonisolated
        await normalGlobalActorAcceptsSendableClosure { print(self) }
    }

    func test_CallerSyncInheritsActorContext_CalleeSyncMainActorIsolated() async {
        // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference7MyActorC023test_CallerSyncInheritse14Context_Calleeh4MainE8IsolatedyyYaF'

        // CHECK-LABEL: // closure #1 in MyActor.test_CallerSyncInheritsActorContext_CalleeSyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await inheritActorContextGlobalActorAcceptsClosure { print(self) } // expected-error {{sending 'self'-isolated value of type '() -> ()' with later accesses to main actor-isolated context risks causing data races}}

        // This is a synchronous closure, so we error here.
        //
        // CHECK-LABEL: // closure #2 in MyActor.test_CallerSyncInheritsActorContext_CalleeSyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await inheritActorContextGlobalActorAcceptsSendingClosure { print(self) } // expected-error {{'self'-isolated value of type '() -> ()' passed as a strongly transferred parameter}}

        // CHECK-LABEL: // closure #3 in MyActor.test_CallerSyncInheritsActorContext_CalleeSyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await inheritActorContextGlobalActorAcceptsSendableClosure { print(self) }
    }
}

//////////////////////////////////////////////////
// MARK: Actor Instance Sync User Async Closure //
//////////////////////////////////////////////////

extension MyActor {

    func test_CallerSyncNormal_CalleeAsyncNonIsolated() async {
        // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference7MyActorC44test_CallerSyncNormal_CalleeAsyncNonIsolatedyyYaF'

        // CHECK-LABEL: closure #1 in MyActor.test_CallerSyncNormal_CalleeAsyncNonIsolated()
        // CHECK-NEXT: Isolation: actor_instance. name: 'self'
        normalAcceptsAsyncClosure { print(self) }

        // CHECK-LABEL: closure #2 in MyActor.test_CallerSyncNormal_CalleeAsyncNonIsolated()
        // CHECK-NEXT: Isolation: nonisolated
        normalAcceptsSendingAsyncClosure { print(self) }

        // CHECK-LABEL: // closure #3 in MyActor.test_CallerSyncNormal_CalleeAsyncNonIsolated()
        // CHECK-NEXT: // Isolation: nonisolated
        normalAcceptsSendableAsyncClosure { print(self) }
    }

    func test_CallerSyncInheritsActorContext_CalleeAsyncNonisolated() {
        // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference7MyActorC023test_CallerSyncInheritsE30Context_CalleeAsyncNonisolatedyyF'

        // CHECK-LABEL: // closure #1 in MyActor.test_CallerSyncInheritsActorContext_CalleeAsyncNonisolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        inheritActorContextAcceptsAsyncClosure { print(self) }

        // CHECK-LABEL: // closure #2 in MyActor.test_CallerSyncInheritsActorContext_CalleeAsyncNonisolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        inheritActorContextAcceptsSendingAsyncClosure { print(self) }

        // CHECK-LABEL: // closure #3 in MyActor.test_CallerSyncInheritsActorContext_CalleeAsyncNonisolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        inheritActorContextAcceptsSendableAsyncClosure { print(self) }
    }

    func test_CallerSyncNormal_CalleeAsyncMainActorIsolated() async {
        // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference7MyActorC037test_CallerSyncNormal_CalleeAsyncMainE8IsolatedyyYaF'

        // CHECK-LABEL: // closure #1 in MyActor.test_CallerSyncNormal_CalleeAsyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await normalGlobalActorAcceptsAsyncClosure { print(self) } // expected-error {{sending 'self'-isolated value of type '() async -> ()' with later accesses to main actor-isolated context risks causing data races}}

        // CHECK-LABEL: // closure #2 in MyActor.test_CallerSyncNormal_CalleeAsyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: nonisolated
        await normalGlobalActorAcceptsSendingAsyncClosure { print(self) }

        // CHECK-LABEL: // closure #3 in MyActor.test_CallerSyncNormal_CalleeAsyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: nonisolated
        await normalGlobalActorAcceptsSendableAsyncClosure { print(self) }
    }

    func test_CallerSyncInheritsActorContext_CalleeAsyncMainActorIsolated() async {
        // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference7MyActorC023test_CallerSyncInheritse23Context_CalleeAsyncMainE8IsolatedyyYaF'

        // CHECK-LABEL: // closure #1 in MyActor.test_CallerSyncInheritsActorContext_CalleeAsyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await inheritActorContextGlobalActorAcceptsAsyncClosure { print(self) }

        // CHECK-LABEL: // closure #2 in MyActor.test_CallerSyncInheritsActorContext_CalleeAsyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await inheritActorContextGlobalActorAcceptsSendingAsyncClosure { print(self) }

        // CHECK-LABEL: // closure #3 in MyActor.test_CallerSyncInheritsActorContext_CalleeAsyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await inheritActorContextGlobalActorAcceptsSendableAsyncClosure { print(self) }
    }
}

////////////////////////////////////////////////////////
// MARK: Actor Instance Async User Sync Closure Tests //
////////////////////////////////////////////////////////

extension MyActor {

    func test_CallerAsyncNormal_CalleeSyncNonIsolated() async {
        // Just to help start our pattern matchin since the closure #$NUM line
        // appears in other parts of the file.
        // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference7MyActorC44test_CallerAsyncNormal_CalleeSyncNonIsolatedyyYaF'

        // CHECK-LABEL: closure #1 in MyActor.test_CallerAsyncNormal_CalleeSyncNonIsolated()
        // CHECK-NEXT: Isolation: actor_instance. name: 'self'
        await asyncNormalAcceptsClosure { print(self) } // expected-error {{sending 'self'-isolated value of type '() -> ()' with later accesses to nonisolated context risks causing data races}}

        // CHECK-LABEL: closure #2 in MyActor.test_CallerAsyncNormal_CalleeSyncNonIsolated()
        // CHECK-NEXT: Isolation: nonisolated
        await asyncNormalAcceptsSendingClosure { print(self) }

        // CHECK-LABEL: // closure #3 in MyActor.test_CallerAsyncNormal_CalleeSyncNonIsolated()
        // CHECK-NEXT: // Isolation: nonisolated
        await asyncNormalAcceptsSendableClosure { print(self) }
    }

    func test_CallerAsyncInheritsActorContext_CalleeSyncNonisolated() async {
        // CHECK: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference7MyActorC024test_CallerAsyncInheritsE29Context_CalleeSyncNonisolatedyyYaF'

        // CHECK-LABEL: // closure #1 in MyActor.test_CallerAsyncInheritsActorContext_CalleeSyncNonisolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await asyncInheritActorContextAcceptsClosure { print(self) } // expected-error {{sending 'self'-isolated value of type '() -> ()' with later accesses to nonisolated context risks causing data races}}

        // This is a synchronous closure, so we error here.
        //
        // CHECK-LABEL: // closure #2 in MyActor.test_CallerAsyncInheritsActorContext_CalleeSyncNonisolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await asyncInheritActorContextAcceptsSendingClosure { print(self) } // expected-error {{'self'-isolated value of type '() -> ()' passed as a strongly transferred parameter}}

        // CHECK-LABEL: // closure #3 in MyActor.test_CallerAsyncInheritsActorContext_CalleeSyncNonisolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await asyncInheritActorContextAcceptsSendableClosure { print(self) }
    }

    func test_CallerAsyncNormal_CalleeSyncMainActorIsolated() async {
        // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference7MyActorC037test_CallerAsyncNormal_CalleeSyncMainE8IsolatedyyYaF'

        // CHECK-LABEL: // closure #1 in MyActor.test_CallerAsyncNormal_CalleeSyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await asyncNormalGlobalActorAcceptsClosure { print(self) } // expected-error {{sending 'self'-isolated value of type '() -> ()' with later accesses to main actor-isolated context risks causing data races}}

        // CHECK-LABEL: // closure #2 in MyActor.test_CallerAsyncNormal_CalleeSyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: nonisolated
        await asyncNormalGlobalActorAcceptsSendingClosure { print(self) }

        // CHECK-LABEL: // closure #3 in MyActor.test_CallerAsyncNormal_CalleeSyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: nonisolated
        await asyncNormalGlobalActorAcceptsSendableClosure { print(self) }
    }

    func test_CallerAsyncInheritsActorContext_CalleeSyncMainActorIsolated() async {
        // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference7MyActorC024test_CallerAsyncInheritse22Context_CalleeSyncMainE8IsolatedyyYaF'

        // CHECK-LABEL: // closure #1 in MyActor.test_CallerAsyncInheritsActorContext_CalleeSyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await asyncInheritActorContextGlobalActorAcceptsClosure { print(self) } // expected-error {{sending 'self'-isolated value of type '() -> ()' with later accesses to main actor-isolated context risks causing data races}}

        // This is a synchronous function, so we error.
        //
        // CHECK-LABEL: // closure #2 in MyActor.test_CallerAsyncInheritsActorContext_CalleeSyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await asyncInheritActorContextGlobalActorAcceptsSendingClosure { print(self) } // expected-error {{'self'-isolated value of type '() -> ()' passed as a strongly transferred parameter}}

        // CHECK-LABEL: // closure #3 in MyActor.test_CallerAsyncInheritsActorContext_CalleeSyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await asyncInheritActorContextGlobalActorAcceptsSendableClosure { print(self) }
    }
}

///////////////////////////////////////////////////
// MARK: Actor Instance Async User Async Closure //
///////////////////////////////////////////////////

extension MyActor {

    func test_CallerAsyncNormal_CalleeAsyncNonIsolated() async {
        // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference7MyActorC029test_CallerAsyncNormal_CalleeH11NonIsolatedyyYaF'

        // CHECK-LABEL: closure #1 in MyActor.test_CallerAsyncNormal_CalleeAsyncNonIsolated()
        // CHECK-NEXT: Isolation: actor_instance. name: 'self'
        await asyncNormalAcceptsAsyncClosure { print(self) } // expected-error {{sending 'self'-isolated value of type '() async -> ()' with later accesses to nonisolated context risks causing data races}}

        // CHECK-LABEL: closure #2 in MyActor.test_CallerAsyncNormal_CalleeAsyncNonIsolated()
        // CHECK-NEXT: Isolation: nonisolated
        await asyncNormalAcceptsSendingAsyncClosure { print(self) }

        // CHECK-LABEL: // closure #3 in MyActor.test_CallerAsyncNormal_CalleeAsyncNonIsolated()
        // CHECK-NEXT: // Isolation: nonisolated
        await asyncNormalAcceptsSendableAsyncClosure { print(self) }
    }

    func test_CallerAsyncInheritsActorContext_CalleeAsyncNonisolated() async {
        // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference7MyActorC024test_CallerAsyncInheritse14Context_CalleeH11NonisolatedyyYaF'

        // CHECK-LABEL: // closure #1 in MyActor.test_CallerAsyncInheritsActorContext_CalleeAsyncNonisolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await asyncInheritActorContextAcceptsAsyncClosure { print(self) }

        // CHECK-LABEL: // closure #2 in MyActor.test_CallerAsyncInheritsActorContext_CalleeAsyncNonisolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await asyncInheritActorContextAcceptsSendingAsyncClosure { print(self) }

        // CHECK-LABEL: // closure #3 in MyActor.test_CallerAsyncInheritsActorContext_CalleeAsyncNonisolated()
        // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
        await asyncInheritActorContextAcceptsSendingAsyncClosure { @CustomActor in print(self) }

        // CHECK-LABEL: // closure #4 in MyActor.test_CallerAsyncInheritsActorContext_CalleeAsyncNonisolated()
        // CHECK-NEXT: // Isolation: global_actor. type: MainActor
        await asyncInheritActorContextAcceptsSendingAsyncClosure { @MainActor in print(self) }

        // CHECK-LABEL: // closure #5 in MyActor.test_CallerAsyncInheritsActorContext_CalleeAsyncNonisolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await asyncInheritActorContextAcceptsSendableAsyncClosure { print(self) }
    }

    func test_CallerAsyncNormal_CalleeAsyncMainActorIsolated() async {
        // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference7MyActorC029test_CallerAsyncNormal_Calleeh4MainE8IsolatedyyYaF'

        // CHECK-LABEL: // closure #1 in MyActor.test_CallerAsyncNormal_CalleeAsyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await asyncNormalGlobalActorAcceptsAsyncClosure { print(self) } // expected-error {{sending 'self'-isolated value of type '() async -> ()' with later accesses to main actor-isolated context risks causing data races}}

        // CHECK-LABEL: // closure #2 in MyActor.test_CallerAsyncNormal_CalleeAsyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: nonisolated
        await asyncNormalGlobalActorAcceptsSendingAsyncClosure { print(self) }

        // CHECK-LABEL: // closure #3 in MyActor.test_CallerAsyncNormal_CalleeAsyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: nonisolated
        await asyncNormalGlobalActorAcceptsSendableAsyncClosure { print(self) }
    }

    func test_CallerAsyncInheritsActorContext_CalleeAsyncMainActorIsolated() async {
        // CHECK-LABEL: } // end sil function '$s54transfernonsendable_closureliterals_isolationinference7MyActorC024test_CallerAsyncInheritse14Context_Calleeh4MainE8IsolatedyyYaF'

        // CHECK-LABEL: // closure #1 in MyActor.test_CallerAsyncInheritsActorContext_CalleeAsyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await asyncInheritActorContextGlobalActorAcceptsAsyncClosure { print(self) }

        // CHECK-LABEL: // closure #2 in MyActor.test_CallerAsyncInheritsActorContext_CalleeAsyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await asyncInheritActorContextGlobalActorAcceptsSendingAsyncClosure { print(self) }

        // CHECK-LABEL: // closure #3 in MyActor.test_CallerAsyncInheritsActorContext_CalleeAsyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: global_actor. type: CustomActor
        await asyncInheritActorContextGlobalActorAcceptsSendingAsyncClosure { @CustomActor in print(self) }

        // CHECK-LABEL: // closure #4 in MyActor.test_CallerAsyncInheritsActorContext_CalleeAsyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: global_actor. type: MainActor
        await asyncInheritActorContextGlobalActorAcceptsSendingAsyncClosure { @MainActor in print(self) }

        // CHECK-LABEL: // closure #5 in MyActor.test_CallerAsyncInheritsActorContext_CalleeAsyncMainActorIsolated()
        // CHECK-NEXT: // Isolation: actor_instance. name: 'self'
        await asyncInheritActorContextGlobalActorAcceptsSendableAsyncClosure { print(self) }
    }
}
