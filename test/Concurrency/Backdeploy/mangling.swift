// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -target %target-cpu-apple-macosx12.0 -module-name main -emit-ir -o %t/new.ir
// RUN: %FileCheck %s --check-prefix=NEW < %t/new.ir
// RUN: %target-swift-frontend %s -target %target-cpu-apple-macosx10.15 -module-name main -emit-ir -o %t/old.ir -disable-availability-checking
// RUN: %FileCheck %s --check-prefix=OLD < %t/old.ir

// Check that we add extra type metadata accessors for new kinds of functions
// when back-deploying. These are used instead of using demangling cache
// variables since old runtimes cannot synthesize type metadata based on the
// new mangling.

// RUN: %target-build-swift -target %target-cpu-apple-macosx10.15 %s -o %t/test_mangling -Xfrontend -disable-availability-checking
// RUN: %target-run %t/test_mangling

// REQUIRES: OS=macosx
// REQUIRES: executable_test
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: freestanding

actor MyActor { }

protocol MyProtocol {
  associatedtype AssocSendable
  associatedtype AssocAsync
  associatedtype AssocGlobalActor
  associatedtype AssocIsolated
}

typealias SendableFn = @Sendable () -> Void
typealias AsyncFn = () async -> Void
typealias GlobalActorFn = @MainActor () -> Void
typealias ActorIsolatedFn = (isolated MyActor) -> String

struct MyStruct: MyProtocol {
  typealias AssocSendable = SendableFn
  typealias AssocAsync = AsyncFn
  typealias AssocGlobalActor = GlobalActorFn
  typealias AssocIsolated = ActorIsolatedFn
}

func assocSendable<T: MyProtocol>(_: T.Type) -> Any.Type { return T.AssocSendable.self }
func assocAsync<T: MyProtocol>(_: T.Type) -> Any.Type { return T.AssocAsync.self }
func assocGlobalActor<T: MyProtocol>(_: T.Type) -> Any.Type { return T.AssocGlobalActor.self }
func assocIsolated<T: MyProtocol>(_: T.Type) -> Any.Type { return T.AssocIsolated.self }

assert(assocSendable(MyStruct.self) == SendableFn.self)
assert(assocAsync(MyStruct.self) == AsyncFn.self)
assert(assocGlobalActor(MyStruct.self) == GlobalActorFn.self)
assert(assocIsolated(MyStruct.self) == ActorIsolatedFn.self)

// type metadata accessor for @Sendable () -> ()
// OLD: define linkonce_odr hidden swiftcc %swift.metadata_response @"$syyYbcMa"
// NEW-NOT: define linkonce_odr hidden swiftcc %swift.metadata_response @"$syyYbcMa"

// type metadata accessor for () async -> ()
// OLD: define linkonce_odr hidden swiftcc %swift.metadata_response @"$syyYacMa"
// NEW-NOT: define linkonce_odr hidden swiftcc %swift.metadata_response @"$syyYacMa"

// type metadata accessor for @MainActor () -> ()
// OLD: define linkonce_odr hidden swiftcc %swift.metadata_response @"$syyScMYccMa"
// NEW-NOT: define linkonce_odr hidden swiftcc %swift.metadata_response @"$syyScMYccMa"

// OLD: call swiftcc %swift.metadata_response @"$syyYbcMa"
// OLD-NOT: call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr @"$syyYbcMD")

// NEW-NOT: call swiftcc %swift.metadata_response @"$syyYbcMa"
// NEW: call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr @"$syyYbcMD")

// OLD: call swiftcc %swift.metadata_response @"$syyYacMa"
// OLD-NOT: ptr @__swift_instantiateConcreteTypeFromMangledName(ptr @"$syyYacMD")

// NEW-NOT: call swiftcc %swift.metadata_response @"$syyYacMa"
// NEW: call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr @"$syyYacMD")

// OLD: call swiftcc %swift.metadata_response @"$syyScMYccMa"
// OLD-NOT: call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr @"$syyScMYccMD")

// NEW-NOT: call swiftcc %swift.metadata_response @"$syyScMYccMa"
// NEW: call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr @"$syyScMYccMD")

// OLD: call swiftcc %swift.metadata_response @"$sSS4main7MyActorCYicMa"(i64 0)
// OLD-NOT: call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr @"$sSS4main7MyActorCYicMD")

// NEW-NOT: call swiftcc %swift.metadata_response @"$sSS4main7MyActorCYicMa"(i64 0)
// NEW: call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr @"$sSS4main7MyActorCYicMD")
