// RUN: %empty-directory(%t)
// RUN: %swift-frontend %s -swift-version 6 -module-name main -emit-ir -o %t/new.ir
// RUN: %FileCheck %s --check-prefix=NEW < %t/new.ir
// RUN: %target-swift-frontend %s -target %target-cpu-apple-macosx10.15 -module-name main -emit-ir -o %t/old.ir
// RUN: %FileCheck %s --check-prefix=OLD < %t/old.ir

// Check that we add extra type metadata accessors for types with generic
// parameters that have an inverse. These are used instead of using demangling
// cache variables since old runtimes cannot synthesize type metadata based on
// the new mangling.

// RUN: %target-build-swift -target %target-cpu-apple-macosx10.15 %s -o %t/test_mangling
// RUN: %target-run %t/test_mangling | %FileCheck %s

// REQUIRES: OS=macosx
// REQUIRES: executable_test


// This type's generic parameter is noncopyable, so older runtimes can't
// demangle the type's name to build the metadata.
struct Foo<T: ~Copyable>: ~Copyable {
  mutating func bar() { print("Foo.bar") }
}

func test() {
  var foo = Foo<Int>()
  foo.bar()
}
test()
// CHECK: Foo.bar

// NEW: define hidden swiftcc void @"$s4main4testyyF"()
// NEW-NOT: %swift.metadata_response
// NEW: call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr @"$s4main3FooVySiGMD")
// NEW-NOT: %swift.metadata_response
// NEW: }

// OLD: define hidden swiftcc void @"$s4main4testyyF"()
// OLD-NOT: __swift_instantiateConcreteTypeFromMangledName
// OLD: call swiftcc %swift.metadata_response @"$s4main3FooVySiGMa"(i64 0)
// OLD-NOT: __swift_instantiateConcreteTypeFromMangledName
// OLD: }


// This type does not need a Swift 6.0 runtime, despite being noncopyable,
// because it doesn't have a noncopyable generic parameter.
struct JustNoncopyable<T>: ~Copyable {
  mutating func bar() { print("JustNoncopyable.bar") }
}

func testNonGeneric() {
    var ng = JustNoncopyable<Int>()
    ng.bar()
}
testNonGeneric()

// CHECK: JustNoncopyable.bar

// NEW: define hidden swiftcc void @"$s4main14testNonGenericyyF"()
// NEW-NOT: %swift.metadata_response
// NEW: call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr @"$s4main15JustNoncopyableVySiGMD")
// NEW-NOT: %swift.metadata_response
// NEW: }

// OLD: define hidden swiftcc void @"$s4main14testNonGenericyyF"()
// OLD-NOT: %swift.metadata_response
// OLD: call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr @"$s4main15JustNoncopyableVySiGMD")
// OLD-NOT: %swift.metadata_response
// OLD: }
