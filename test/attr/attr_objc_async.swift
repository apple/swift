// RUN: %target-swift-frontend -disable-objc-attr-requires-foundation-module -typecheck -verify -verify-ignore-unknown %s -swift-version 5 -enable-source-import -I %S/Inputs  -disable-availability-checking -warn-concurrency
// RUN: %target-swift-ide-test -skip-deinit=false -print-ast-typechecked -source-filename %s -function-definitions=true -prefer-type-repr=false -print-implicit-attrs=true -explode-pattern-binding-decls=true -disable-objc-attr-requires-foundation-module -swift-version 5 -enable-source-import -I %S/Inputs  | %FileCheck %s
// RUN: not %target-swift-frontend -typecheck -dump-ast -disable-objc-attr-requires-foundation-module %s -swift-version 5 -enable-source-import -I %S/Inputs  -disable-availability-checking -warn-concurrency > %t.ast
// RUN: %FileCheck -check-prefix CHECK-DUMP %s < %t.ast
// REQUIRES: objc_interop
// REQUIRES: concurrency
import Foundation

// CHECK: class MyClass
class MyClass {
  // CHECK: @objc func doBigJob() async -> Int
  // CHECK-DUMP: func_decl{{.*}}doBigJob{{.*}}foreign_async=@convention(block) (Int) -> (),completion_handler_param=0
  @objc func doBigJob() async -> Int { return 0 }

  // CHECK: @objc func doBigJobOrFail(_: Int) async throws -> (AnyObject, Int)
  // CHECK-DUMP: func_decl{{.*}}doBigJobOrFail{{.*}}foreign_async=@convention(block) (Optional<AnyObject>, Int, Optional<any Error>) -> (),completion_handler_param=1,error_param=2
  @objc func doBigJobOrFail(_: Int) async throws -> (AnyObject, Int) { return (self, 0) }

  @objc func takeAnAsync(_ fn: () async -> Int) { } // expected-error{{method cannot be marked @objc because the type of the parameter cannot be represented in Objective-C}}
  // expected-note@-1{{'async' function types cannot be represented in Objective-C}}

  @objc class func createAsynchronously() async -> Self? { nil }
  // expected-error@-1{{asynchronous method returning 'Self' cannot be '@objc'}}
}

// actor exporting Objective-C entry points.

// CHECK: actor MyActor
actor MyActor {
  // CHECK: @objc func doBigJob() async -> Int
  // CHECK-DUMP: func_decl{{.*}}doBigJob{{.*}}foreign_async=@convention(block) (Int) -> (),completion_handler_param=0
  @objc func doBigJob() async -> Int { return 0 }

  // CHECK: @objc func doBigJobOrFail(_: Int) async throws -> (AnyObject, Int)
  // CHECK-DUMP: func_decl{{.*}}doBigJobOrFail{{.*}}foreign_async=@convention(block) (Optional<AnyObject>, Int, Optional<any Error>) -> (),completion_handler_param=1,error_param=2
  @objc func doBigJobOrFail(_: Int) async throws -> (AnyObject, Int) { return (self, 0) }
  // expected-warning@-1{{non-sendable type '(AnyObject, Int)' returned by actor-isolated '@objc' instance method 'doBigJobOrFail' cannot cross actor boundary}}

  // Actor-isolated entities cannot be exposed to Objective-C.
  @objc func synchronousBad() { } // expected-error{{actor-isolated instance method 'synchronousBad()' cannot be @objc}}
  // expected-note@-1{{add 'async' to function 'synchronousBad()' to make it asynchronous}} {{30-30= async}}

  @objc var badProp: AnyObject { self } // expected-error{{actor-isolated property 'badProp' cannot be @objc}}
  @objc subscript(index: Int) -> AnyObject { self } // expected-error{{actor-isolated subscript 'subscript(_:)' cannot be @objc}}

  // CHECK: @objc nonisolated func synchronousGood()
  @objc nonisolated func synchronousGood() { }
}

actor class MyActor2 { }
// expected-error@-1 {{keyword 'class' cannot be used as an identifier here}}

// CHECK: @objc actor MyObjCActor
@objc actor MyObjCActor: NSObject { }

@objc actor class MyObjCActor2: NSObject {}
// expected-error@-1 {{keyword 'class' cannot be used as an identifier here}}
