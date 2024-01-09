// RUN: %target-swift-frontend -emit-silgen -verify %s
// RUN: %target-swift-emit-module-interface(%t.swiftinterface) -DEMIT_IFACE %s -module-name IsolatedDeinitCompatibility
// RUN: %target-swift-typecheck-module-from-interface(%t.swiftinterface) -module-name IsolatedDeinitCompatibility
// RUN: %FileCheck %s < %t.swiftinterface

func dummy() async {}

// MARK: Sync deinit in class

// CHECK-NOT: #
// CHECK: open class SyncClassDefaultOpen {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: }
open class SyncClassDefaultOpen {
    deinit {}
}

// CHECK-NOT: #
// CHECK: public class SyncClassDefaultPublic {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: }
public class SyncClassDefaultPublic {
    deinit {}
}

// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: open class SyncClassGlobalActorOpen {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}@_Concurrency.MainActor deinit
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: public class SyncClassGlobalActorOpen {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #endif
open class SyncClassGlobalActorOpen {
    @MainActor deinit {}
}

// CHECK-NOT: #
// CHECK: public class SyncClassGlobalActorPublic {
// CHECK-NOT: #
// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: {{(@objc )?}}@_Concurrency.MainActor deinit
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: #endif
// CHECK-NOT: #
// CHECK: }
public class SyncClassGlobalActorPublic {
    @MainActor deinit {}
}

// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: @_Concurrency.MainActor open class SyncClassIsolatedOpen {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}isolated deinit
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: @_Concurrency.MainActor public class SyncClassIsolatedOpen {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #endif
@MainActor
open class SyncClassIsolatedOpen {
    isolated deinit {}
}

// CHECK-NOT: #
// CHECK: @_Concurrency.MainActor public class SyncClassIsolatedPublic {
// CHECK-NOT: #
// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: {{(@objc )?}}isolated deinit
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: #endif
// CHECK-NOT: #
// CHECK: }
@MainActor
public class SyncClassIsolatedPublic {
    isolated deinit {}
}

// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: @_Concurrency.MainActor open class SyncClassNonisolatedOpen {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}nonisolated deinit
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: @_Concurrency.MainActor public class SyncClassNonisolatedOpen {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #endif
@MainActor
open class SyncClassNonisolatedOpen {
    nonisolated deinit {}
}

// CHECK-NOT: #
// CHECK: @_Concurrency.MainActor public class SyncClassNonisolatedPublic {
// CHECK-NOT: #
// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: {{(@objc )?}}nonisolated deinit
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: #endif
// CHECK-NOT: #
// CHECK: }
@MainActor
public class SyncClassNonisolatedPublic {
    nonisolated deinit {}
}

// MARK: Sync deinit in actor

// CHECK: #if {{.*}}$Actors
// CHECK: public actor SyncActorDefaultPublic {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #endif
public actor SyncActorDefaultPublic {
    deinit {}
}

// CHECK: #if {{.*}}$Actors
// CHECK: public actor SyncActorGlobalActorPublic {
// CHECK-NOT: #
// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: {{(@objc )?}}@_Concurrency.MainActor deinit
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: #endif
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #endif
public actor SyncActorGlobalActorPublic {
    @MainActor deinit {}
}

// CHECK: #if {{.*}}$Actors
// CHECK: public actor SyncActorIsolatedPublic {
// CHECK-NOT: #
// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: {{(@objc )?}}isolated deinit
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: #endif
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #endif
public actor SyncActorIsolatedPublic {
    isolated deinit {}
}

// CHECK: #if {{.*}}$Actors
// CHECK: public actor SyncActorNonisolatedPublic {
// CHECK-NOT: #
// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: {{(@objc )?}}nonisolated deinit
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: #endif
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #endif
public actor SyncActorNonisolatedPublic {
    nonisolated deinit {}
}

// MARK: Async deinit in class

// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: open class AsyncClassDefaultOpen {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit async
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: public class AsyncClassDefaultOpen {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #endif
open class AsyncClassDefaultOpen {
    deinit async { await dummy() }
}

// CHECK-NOT: #
// CHECK: public class AsyncClassDefaultPublic {
// CHECK-NOT: #
// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit async
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: #endif
// CHECK-NOT: #
// CHECK: }
public class AsyncClassDefaultPublic {
    deinit async { await dummy() }
}

// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: open class AsyncClassGlobalActorOpen {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}@_Concurrency.MainActor deinit async
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: public class AsyncClassGlobalActorOpen {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #endif
open class AsyncClassGlobalActorOpen {
    @MainActor deinit async { await dummy() }
}

// CHECK-NOT: #
// CHECK: public class AsyncClassGlobalActorPublic {
// CHECK-NOT: #
// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: {{(@objc )?}}@_Concurrency.MainActor deinit async
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: #endif
// CHECK-NOT: #
// CHECK: }
public class AsyncClassGlobalActorPublic {
    @MainActor deinit async { await dummy() }
}

// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: @_Concurrency.MainActor open class AsyncClassIsolatedOpen {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}isolated deinit async
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: @_Concurrency.MainActor public class AsyncClassIsolatedOpen {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #endif
@MainActor
open class AsyncClassIsolatedOpen {
    isolated deinit async { await dummy() }
}

// CHECK-NOT: #
// CHECK: public class AsyncClassIsolatedPublic {
// CHECK-NOT: #
// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: {{(@objc )?}}isolated deinit async
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: #endif
// CHECK-NOT: #
// CHECK: }
@MainActor
public class AsyncClassIsolatedPublic {
    isolated deinit async { await dummy() }
}

// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: @_Concurrency.MainActor open class AsyncClassNonisolatedOpen {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}nonisolated deinit async
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: @_Concurrency.MainActor public class AsyncClassNonisolatedOpen {
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #endif
@MainActor
open class AsyncClassNonisolatedOpen {
    nonisolated deinit async { await dummy() }
}

// CHECK-NOT: #
// CHECK: public class AsyncClassNonisolatedPublic {
// CHECK-NOT: #
// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: {{(@objc )?}}nonisolated deinit async
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: #endif
// CHECK-NOT: #
// CHECK: }
@MainActor
public class AsyncClassNonisolatedPublic {
    nonisolated deinit async { await dummy() }
}

// MARK: Async deinit in actor

// CHECK: #if {{.*}}$Actors
// CHECK: public actor AsyncActorDefaultPublic {
// CHECK-NOT: #
// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit async
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: #endif
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #endif
public actor AsyncActorDefaultPublic {
    deinit async { await dummy() }
}

// CHECK: #if {{.*}}$Actors
// CHECK: public actor AsyncActorGlobalActorPublic {
// CHECK-NOT: #
// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: {{(@objc )?}}@_Concurrency.MainActor deinit async
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: #endif
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #endif
public actor AsyncActorGlobalActorPublic {
    @MainActor deinit async { await dummy() }
}

// CHECK: #if {{.*}}$Actors
// CHECK: public actor AsyncActorIsolatedPublic {
// CHECK-NOT: #
// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: {{(@objc )?}}isolated deinit async
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: #endif
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #endif
public actor AsyncActorIsolatedPublic {
    isolated deinit async { await dummy() }
}

// CHECK: #if {{.*}}$Actors
// CHECK: public actor AsyncActorNonisolatedPublic {
// CHECK-NOT: #
// CHECK: #if {{.*}}$IsolatedDeinit
// CHECK-NOT: #
// CHECK: {{(@objc )?}}nonisolated deinit async
// CHECK-NOT: #
// CHECK: #else
// CHECK-NOT: #
// CHECK: {{(@objc )?}}deinit
// CHECK-NOT: #
// CHECK: #endif
// CHECK-NOT: #
// CHECK: }
// CHECK-NOT: #
// CHECK: #endif
public actor AsyncActorNonisolatedPublic {
    nonisolated deinit async { await dummy() }
}

// MARK: - Open actor

// Check that open actors are not allowed
// If they become allowed in the future, extra test cases need to be added to this test
#if !EMIT_IFACE
open actor OpenActor {} // expected-error {{only classes and overridable class members can be declared 'open'; use 'public'}}
#endif
