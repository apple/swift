// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/StrictModule.swiftmodule -module-name StrictModule -swift-version 6 %S/Inputs/StrictModule.swift
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/NonStrictModule.swiftmodule -module-name NonStrictModule %S/Inputs/NonStrictModule.swift
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/OtherActors.swiftmodule -module-name OtherActors %S/Inputs/OtherActors.swift -disable-availability-checking

// RUN: %target-swift-frontend  -I %t %s -emit-sil -o /dev/null -verify  -parse-as-library -enable-upcoming-feature GlobalConcurrency
// RUN: %target-swift-frontend  -I %t %s -emit-sil -o /dev/null -verify -strict-concurrency=targeted -parse-as-library -enable-upcoming-feature GlobalConcurrency
// RUN: %target-swift-frontend  -I %t %s -emit-sil -o /dev/null -verify -strict-concurrency=complete  -parse-as-library -enable-upcoming-feature GlobalConcurrency

// REQUIRES: concurrency
// REQUIRES: asserts

@preconcurrency import NonStrictModule
@_predatesConcurrency import StrictModule // expected-warning{{'@_predatesConcurrency' has been renamed to '@preconcurrency'}}
@preconcurrency import OtherActors
// expected-warning@-1{{'@preconcurrency' attribute on module 'OtherActors' has no effect}}{{1-17=}}

@preconcurrency
class MyPredatesConcurrencyClass { }

enum EnumWithPredatesConcurrencyValue {
  case stored(MyPredatesConcurrencyClass)
}

func acceptSendable<T: Sendable>(_: T) { }

@available(SwiftStdlib 5.1, *)
func test(
  ss: StrictStruct, ns: NonStrictClass, oma: OtherModuleActor,
  ssOpt: StrictStruct?, nsOpt: NonStrictClass?,
  ssc: SomeSendableClass,
  mpcc: MyPredatesConcurrencyClass
) async {
  acceptSendable(ss) // expected-warning{{type 'StrictStruct' does not conform to the 'Sendable' protocol}}
  acceptSendable(ns) // silence issue entirely
  acceptSendable(ssOpt) // expected-warning{{type 'StrictStruct' does not conform to the 'Sendable' protocol}}
  acceptSendable(nsOpt) // silence issue entirely
  acceptSendable(oma) // okay
  acceptSendable(ssc) // okay
  acceptSendable(mpcc)
}

let nonStrictGlobal = NonStrictClass() // no warning

let strictGlobal = StrictStruct() // expected-warning{{let 'strictGlobal' is not concurrency-safe because non-'Sendable' type 'StrictStruct' may have shared mutable state}}
// expected-note@-1{{disable concurrency-safety checks if accesses are protected by an external synchronization mechanism}}
// expected-note@-2{{annotate 'strictGlobal' with '@MainActor' if property should only be accessed from the main actor}}

extension NonStrictClass {
  @Sendable func f() { }
}

extension StrictStruct {
  @Sendable func f() { } // expected-warning{{instance method of non-Sendable type 'StrictStruct' cannot be marked as '@Sendable'}}
}


struct HasStatics {
  nonisolated static let ns: NonStrictClass = NonStrictClass()

  nonisolated static let ss: StrictStruct = StrictStruct()
  // expected-warning@-1{{'nonisolated' can not be applied to variable with non-'Sendable' type 'StrictStruct'}}
  // expected-warning@-2{{static property 'ss' is not concurrency-safe because non-'Sendable' type 'StrictStruct' may have shared mutable state}}
  // expected-note@-3{{disable concurrency-safety checks if accesses are protected by an external synchronization mechanism}}
  // expected-note@-4{{annotate 'ss' with '@MainActor' if property should only be accessed from the main actor}}
}

extension NonStrictClass2: @retroactive MySendableProto { }

extension NonStrictClass3: @retroactive Sendable { }
// expected-warning@-1{{conformance to 'Sendable' must occur in the same source file as class 'NonStrictClass3'; use '@unchecked Sendable' for retroactive conformance}}
