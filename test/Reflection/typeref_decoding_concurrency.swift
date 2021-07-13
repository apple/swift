// REQUIRES: no_asan
// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: libdispatch

// rdar://76038845
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

// SR-12893
// XFAIL: openbsd
// XFAIL: freebsd

// RUN: %empty-directory(%t)

// RUN: %target-build-swift -Xfrontend -enable-anonymous-context-mangled-names %S/Inputs/ConcurrencyTypes.swift -parse-as-library -emit-module -emit-library -module-name TypesToReflect -o %t/%target-library-name(TypesToReflect)
// RUN: %target-build-swift -Xfrontend -enable-anonymous-context-mangled-names %S/Inputs/ConcurrencyTypes.swift %S/Inputs/main.swift -emit-module -emit-executable -module-name TypesToReflect -o %t/TypesToReflect

// RUN: %target-swift-reflection-dump -binary-filename %t/%target-library-name(TypesToReflect) | %FileCheck %s
// RUN: %target-swift-reflection-dump -binary-filename %t/TypesToReflect | %FileCheck %s

// CHECK: FIELDS:
// CHECK: =======
// CHECK: TypesToReflect.UsesConcurrency
// CHECK: ------------------
// CHECK: mainActorFunction: @Swift.MainActor () -> ()
// CHECK: (function
// CHECK:   (global-actor
// CHECK:     (class Swift.MainActor))

// CHECK: actorIsolatedFunction: (isolated TypesToReflect.SomeActor) -> ()
// CHECK: (function
// CHECK:   (parameters
// CHECK:     isolated
// CHECK:     (class TypesToReflect.SomeActor))
