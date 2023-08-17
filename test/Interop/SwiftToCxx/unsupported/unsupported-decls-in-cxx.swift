// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Functions -verify -clang-header-expose-decls=has-expose-attr -disable-availability-checking -emit-clang-header-path %t/functions.h

// RUN: cat %s | grep -v _expose > %t/clean.swift
// RUN: %target-swift-frontend %t/clean.swift -typecheck -module-name Functions -clang-header-expose-decls=all-public -disable-availability-checking -emit-clang-header-path %t/header.h
// RUN: %FileCheck %s < %t/header.h

// REQUIRES: concurrency
// REQUIRES: distributed

// CHECK-NOT: Unsupported
// CHECK: supported

import Distributed

public func supported() {}

@_expose(Cxx)
public actor ActorClass {
    @_expose(Cxx) // expected-error {{actor-isolated instance method 'unsupported()' can not be exposed to C++}}
    public func unsupported() {}

    @_expose(Cxx) // expected-error {{actor-isolated property 'prop' can not be exposed to C++}}
    public var prop: Int { 42 }

    @_expose(Cxx) // ok
    public init() {
    }

    @_expose(Cxx) // ok
    nonisolated public func supported() {}

    @_expose(Cxx) // ok
    nonisolated public var prop2: Int { 42 }

    @_expose(Cxx) // expected-error {{async instance method 'methodAsync()' can not be exposed to C++}}
    public nonisolated func methodAsync() async {
    }
}

@_expose(Cxx)
public distributed actor DistributedActorClass {
    public typealias ActorSystem = LocalTestingDistributedActorSystem

    public init(actorSystem: LocalTestingDistributedActorSystem) {
        self.actorSystem = actorSystem
    }

    @_expose(Cxx) // expected-error {{actor-isolated instance method 'unsupported()' can not be exposed to C++}}
    public func unsupported() {}

    @_expose(Cxx) // expected-error {{actor-isolated property 'prop' can not be exposed to C++}}
    public var prop: Int { 42 }

    @_expose(Cxx) // expected-error {{actor-isolated distributed instance method 'dist()' can not be exposed to C++}}
    distributed public func dist() {}

    @_expose(Cxx) // ok
    public init() {
    }

    @_expose(Cxx) // ok
    nonisolated public func supported() {}

    @_expose(Cxx) // ok
    nonisolated public var prop2: Int { 42 }

    @_expose(Cxx) // expected-error {{nested struct 'NestedStruct' can not yet be represented in C++}}
    public struct NestedStruct {}
}
