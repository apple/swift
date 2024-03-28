// RUN: %empty-directory(%t/src)
// RUN: %empty-directory(%t/sdk)
// RUN: split-file %s %t/src

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -typecheck %t/src/main.swift \
// RUN:   -import-objc-header %t/src/Test.h \
// RUN:   -swift-version 5 \
// RUN:   -enable-experimental-feature SendableCompletionHandlers \
// RUN:   -module-name main -I %t -verify

// REQUIRES: objc_interop
// REQUIRES: asserts

//--- Test.h
#define SWIFT_SENDABLE __attribute__((__swift_attr__("@Sendable")))
#define NONSENDABLE __attribute__((__swift_attr__("@_nonSendable")))
#define MAIN_ACTOR __attribute__((__swift_attr__("@MainActor")))

#pragma clang assume_nonnull begin

@import Foundation;

@interface MyValue : NSObject
@end

SWIFT_SENDABLE
@protocol P <NSObject>
@end

@interface SendableValue : NSObject<P>
@end

SWIFT_SENDABLE
@interface SendableMyValue : MyValue
@end

typedef void (^CompletionHandler)(void (^ SWIFT_SENDABLE)(void)) SWIFT_SENDABLE;

@interface Test : NSObject
-(void) makeRequest:
          (NSString *)typeIdentifier
          loadHandler:(void (^SWIFT_SENDABLE )(void (SWIFT_SENDABLE ^)(void)))loadHandler;
-(void) withSendableId: (void (^)(SWIFT_SENDABLE id)) handler;
-(void) withSendableCustom: (void (^)(MyValue *_Nullable SWIFT_SENDABLE)) handler;
-(void) withNonSendable:(NSString *)operation completionHandler:(void (^ _Nullable NONSENDABLE)(NSString *_Nullable, NSError * _Nullable)) handler;
-(void) withAliasCompletionHandler:(CompletionHandler)handler;
@end

// Placement of SWIFT_SENDABLE matters here
void doSomethingConcurrently(__attribute__((noescape)) void SWIFT_SENDABLE (^block)(void));

@interface TestWithSendableID<T: SWIFT_SENDABLE id> : NSObject
-(void) add: (T) object;
@end

@interface TestWithSendableSuperclass<T: MyValue *SWIFT_SENDABLE> : NSObject
-(void) add: (T) object;
@end

@protocol InnerSendableTypes
-(void) testComposition:(SWIFT_SENDABLE MyValue *)composition;
-(void) test:(NSDictionary<NSString *, SWIFT_SENDABLE id> *)options;
-(void) testWithCallback:(NSString *)name handler:(MAIN_ACTOR void (^)(NSDictionary<NSString *, SWIFT_SENDABLE id> *, NSError * _Nullable))handler;
@end

#pragma clang assume_nonnull end

//--- main.swift

func test_sendable_attr_in_type_context(test: Test) {
  let fn: (String?, (any Error)?) -> Void = { _,_ in }

  test.withNonSendable("", completionHandler: fn) // Ok

  test.makeRequest("id") {
    doSomethingConcurrently($0) // Ok
  }

  test.makeRequest("id") { callback in
    _ = { @Sendable in
      callback() // Ok
    }
  }

  test.withSendableId { id in
    _ = { @Sendable in
      print(id) // Ok
    }
  }

  test.withSendableCustom { val in
    _ = { @Sendable in
      print(val!)
    }
  }

  let _: (@escaping @Sendable (@escaping @Sendable () -> Void) -> Void) -> Void = test.withAliasCompletionHandler

  test.withAliasCompletionHandler { callback in
    doSomethingConcurrently(callback) // Ok
  }

  _ = TestWithSendableID<SendableValue>() // Ok

  TestWithSendableID().add(MyValue())

  TestWithSendableSuperclass().add(SendableMyValue()) // Ok

  TestWithSendableSuperclass().add(MyValue())
}

class TestConformanceWithStripping : InnerSendableTypes {
  func testComposition(_: MyValue) {
    // expected-warning@-1 {{sendability of function types in instance method 'testComposition' does not match requirement in protocol 'InnerSendableTypes'; this is an error in the Swift 6 language mode}}
  }

  func test(_ options: [String: Any]) {
    // expected-warning@-1 {{sendability of function types in instance method 'test' does not match requirement in protocol 'InnerSendableTypes'; this is an error in the Swift 6 language mode}}
  }

  func test(withCallback name: String, handler: @escaping @MainActor ([String : Any], (any Error)?) -> Void) { // Ok
    // expected-warning@-1 {{sendability of function types in instance method 'test(withCallback:handler:)' does not match requirement in protocol 'InnerSendableTypes'; this is an error in the Swift 6 language mode}}
  }
}

class TestConformanceWithoutStripping : InnerSendableTypes {
  func testComposition(_: any MyValue & Sendable) { // Ok
  }

  func test(_ options: [String: any Sendable]) { // Ok
  }

  func test(withCallback name: String, handler: @escaping @MainActor ([String : any Sendable], (any Error)?) -> Void) { // Ok
  }
}
