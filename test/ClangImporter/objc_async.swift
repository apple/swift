// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -typecheck -I %S/Inputs/custom-modules %s -verify -verify-additional-file %swift_src_root/test/Inputs/clang-importer-sdk/usr/include/ObjCConcurrency.h -strict-concurrency=targeted -parse-as-library -enable-experimental-feature SendableCompletionHandlers

// REQUIRES: objc_interop
// REQUIRES: concurrency
// REQUIRES: asserts

import Foundation
import ObjCConcurrency
// expected-remark@-1{{add '@preconcurrency' to suppress 'Sendable'-related warnings from module 'ObjCConcurrency'}}

@available(SwiftStdlib 5.5, *)
@MainActor func onlyOnMainActor() { }

@available(SwiftStdlib 5.5, *)
func testSlowServer(slowServer: SlowServer) async throws {
  let _: Int = await slowServer.doSomethingSlow("mail")
  let _: Bool = await slowServer.checkAvailability()
  let _: String = try await slowServer.findAnswer()
  let _: String = try await slowServer.findAnswerFailingly()

  let (aOpt, b) = try await slowServer.findQAndA()
  if let a = aOpt { // make sure aOpt is optional
    print(a)
  }
  let _: String = b // make sure b is non-optional

  let _: String = try await slowServer.findAnswer()

  let _: Void = await slowServer.doSomethingFun("jump")
  let _: (Int) -> Void = slowServer.completionHandler

  // async version
  let _: Int = await slowServer.doSomethingConflicted("thinking")

  // still async version...
  let _: Int = slowServer.doSomethingConflicted("thinking")
  // expected-error@-1{{expression is 'async' but is not marked with 'await'}}{{16-16=await }}
  // expected-note@-2{{call is 'async'}}

  let _: String? = try await slowServer.fortune()
  let _: Int = try await slowServer.magicNumber(withSeed: 42)

  await slowServer.serverRestart("localhost")
  await slowServer.serverRestart("localhost", atPriority: 0.8)

  _ = await slowServer.allOperations()

  let _: Int = await slowServer.bestName("hello")
  let _: Int = await slowServer.customize("hello")

  slowServer.unavailableMethod() // expected-warning{{instance method 'unavailableMethod' is unavailable from asynchronous contexts}}
  slowServer.unavailableMethodWithMessage() // expected-warning{{instance method 'unavailableMethodWithMessage' is unavailable from asynchronous contexts; Blarpy!}}

  let _: String = await slowServer.dance("slide")
  let _: String = await slowServer.__leap(17)

  slowServer.repeatTrick("jump") // expected-error{{missing argument for parameter 'completionHandler' in call}}

  _ = try await slowServer.someAsyncMethod()


  _ = await slowServer.operations()

  _ = await slowServer.runOnMainThread()
}

@available(SwiftStdlib 5.5, *)
func testSlowServerSynchronous(slowServer: SlowServer) {
  // synchronous version
  let _: Int = slowServer.doSomethingConflicted("thinking")
  slowServer.poorlyNamed("hello") { (i: Int) in print(i) }
  slowServer.customize(with: "hello") { (i: Int) in print(i) }

  slowServer.dance("jig") { s in print(s + "") }
  slowServer.leap(17) { s in print(s + "") }
  slowServer.repeatTrick("jump") { i in print(i + 1) }

  let s = slowServer.operations
  _ = s + []

  slowServer.runOnMainThread { s in
    print(s)
    onlyOnMainActor() // okay because runOnMainThread has a @MainActor closure
  }

  slowServer.overridableButRunsOnMainThread { s in
    print(s)
    onlyOnMainActor() // okay because parameter is @MainActor
  }

  let _: Int = slowServer.overridableButRunsOnMainThread // expected-error{{cannot convert value of type '(((String) -> Void)?) -> Void' to specified type 'Int'}}
}

@available(SwiftStdlib 5.5, *)
func testSlowServerOldSchool(slowServer: SlowServer) {
  slowServer.doSomethingSlow("mail") { i in
    _ = i
  }

  _ = slowServer.allOperations
}

@available(SwiftStdlib 5.5, *)
func testSendable(fn: () -> Void) {
  doSomethingConcurrently(fn) // okay, due to implicit @preconcurrency
  doSomethingConcurrentlyButUnsafe(fn) // okay, @Sendable not part of the type

  var x = 17
  doSomethingConcurrently {
    print(x)
    x = x + 1
  }
}

@available(SwiftStdlib 5.5, *)
func testSendableInAsync() async {
  var x = 17
  doSomethingConcurrentlyButUnsafe {
    x = 42 // expected-warning{{mutation of captured var 'x' in concurrently-executing code}}
  }
  print(x)
}

@available(SwiftStdlib 5.5, *)
func testSendableAttrs(
  sendableClass: SendableClass, nonSendableClass: NonSendableClass,
  sendableEnum: SendableEnum, nonSendableEnum: NonSendableEnum,
  sendableOptions: SendableOptions, nonSendableOptions: NonSendableOptions,
  sendableError: SendableError, nonSendableError: NonSendableError,
  sendableStringEnum: SendableStringEnum, nonSendableStringEnum: NonSendableStringEnum,
  sendableStringStruct: SendableStringStruct, nonSendableStringStruct: NonSendableStringStruct
) async {
  func takesSendable<T: Sendable>(_: T) {}

  takesSendable(sendableClass)        // no-error
  takesSendable(nonSendableClass)     // expected-warning{{conformance of 'NonSendableClass' to 'Sendable' is unavailable}}

  doSomethingConcurrently {
    print(sendableClass)               // no-error
    print(nonSendableClass)            // expected-warning{{capture of 'nonSendableClass' with non-sendable type 'NonSendableClass' in a `@Sendable` closure}}

    print(sendableEnum)                // no-error
    print(nonSendableEnum)             // expected-warning{{capture of 'nonSendableEnum' with non-sendable type 'NonSendableEnum' in a `@Sendable` closure}}

    print(sendableOptions)             // no-error
    print(nonSendableOptions)          // expected-warning{{capture of 'nonSendableOptions' with non-sendable type 'NonSendableOptions' in a `@Sendable` closure}}

    print(sendableError)               // no-error
    print(nonSendableError)            // no-error--we don't respect `@_nonSendable` on `ns_error_domain` types because all errors are Sendable

    print(sendableStringEnum)          // no-error
    print(nonSendableStringEnum)       // expected-warning{{capture of 'nonSendableStringEnum' with non-sendable type 'NonSendableStringEnum' in a `@Sendable` closure}}

    print(sendableStringStruct)        // no-error
    print(nonSendableStringStruct)     // expected-warning{{capture of 'nonSendableStringStruct' with non-sendable type 'NonSendableStringStruct' in a `@Sendable` closure}}
  }
}

// Check import of attributes
@available(SwiftStdlib 5.5, *)
func globalAsync() async { }

@available(SwiftStdlib 5.5, *)
actor MySubclassCheckingSwiftAttributes : ProtocolWithSwiftAttributes {
  func syncMethod() { } // expected-note 2{{calls to instance method 'syncMethod()' from outside of its actor context are implicitly asynchronous}}

  nonisolated func independentMethod() {
    syncMethod() // expected-error{{call to actor-isolated instance method 'syncMethod()' in a synchronous nonisolated context}}
  }

  nonisolated func nonisolatedMethod() {
  }

  @MainActor func mainActorMethod() {
    syncMethod() // expected-error{{call to actor-isolated instance method 'syncMethod()' in a synchronous main actor-isolated context}}
  }

  @MainActor func uiActorMethod() { }
}

// Sendable conformance inference for imported types.
func acceptCV<T: Sendable>(_: T) { }

struct MyStruct: Sendable {
  var range: NSRange
  var inner: SendableStructWithNonSendable
}

@available(SwiftStdlib 5.5, *)
func testCV(r: NSRange, someStruct: SendableStructWithNonSendable) async {
  acceptCV(r)
  acceptCV(someStruct)
}

// Global actor (unsafe) isolation.

@available(SwiftStdlib 5.5, *)
actor SomeActor { }

@available(SwiftStdlib 5.5, *)
@globalActor
struct SomeGlobalActor {
  static let shared = SomeActor()
}

@available(SwiftStdlib 5.5, *)
class MyButton : NXButton {
  @MainActor func testMain() {
    onButtonPress() // okay
  }

  @SomeGlobalActor func testOther() {
    onButtonPress() // expected-error{{call to main actor-isolated instance method 'onButtonPress()' in a synchronous global actor 'SomeGlobalActor'-isolated context}}
  }

  func test() {
    onButtonPress() // okay
  }
}

@available(SwiftStdlib 5.5, *)
func testButtons(mb: MyButton) {
  mb.onButtonPress()
}

@available(SwiftStdlib 5.5, *)
func testMirrored(instance: ClassWithAsync) async {
  await instance.instanceAsync()
  await instance.protocolMethod()
  await instance.customAsyncName()
}

@available(SwiftStdlib 5.5, *)
@MainActor class MyToolbarButton : NXButton {
  var count = 5

  func f() {
    Task {
      let c = count
      print(c)
    }
  }
}

@available(SwiftStdlib 5.5, *)
@MainActor class MyView: NXView {
  func f() {
    Task {
      await self.g()
    }
  }

  func g() async { }
}



@available(SwiftStdlib 5.5, *)
@MainActor func mainActorFn() {}
@available(SwiftStdlib 5.5, *)
@SomeGlobalActor func sgActorFn() {}

// rdar://87217618
// https://github.com/apple/swift/issues/57973
// Check inferred isolation for overridden decls from ObjC. Note that even if
// the override is not present, it can have an affect.
@MainActor
@available(SwiftStdlib 5.5, *)
class FooFrame: PictureFrame {
  init() {
    super.init(size: 0)
  }

  override init(size n: Int) {
    super.init(size: n)
  }

  override func rotate() {
    mainActorFn()
  }
}

@available(SwiftStdlib 5.5, *)
class BarFrame: PictureFrame {
  init() {
    super.init(size: 0)
  }

  override init(size n: Int) {
    super.init(size: n)
  }

  override func rotate() {
    mainActorFn()
  }
}

@available(SwiftStdlib 5.5, *)
@SomeGlobalActor
class BazFrame: NotIsolatedPictureFrame {
  init() {
    super.init(size: 0)
  }

  override init(size n: Int) {
    super.init(size: n)
  }

  override func rotate() {
    sgActorFn()
  }
}

@available(SwiftStdlib 5.5, *)
@SomeGlobalActor
class BazFrameIso: PictureFrame { // expected-error {{global actor 'SomeGlobalActor'-isolated class 'BazFrameIso' has different actor isolation from main actor-isolated superclass 'PictureFrame'}}
}

@available(SwiftStdlib 5.5, *)
func check() async {
  _ = await BarFrame()
  _ = await FooFrame()
  _ = await BazFrame()

  _ = await BarFrame(size: 0)
  _ = await FooFrame(size: 0)
  _ = await BazFrame(size: 0)
}

@available(SwiftStdlib 5.5, *)
func testSender(
  sender: NXSender,
  sendableObject: SendableClass,
  nonSendableObject: NonSendableClass,
  sendableSubclassOfNonSendableObject: NonSendableClass & Sendable,
  sendableProtos: LabellyProtocol & ObjCClub & Sendable,
  nonSendableProtos: LabellyProtocol & ObjCClub,
  sendableGeneric: GenericObject<SendableClass> & Sendable,
  nonSendableGeneric: GenericObject<SendableClass>,
  ptr: UnsafeMutableRawPointer,
  stringArray: [String]
) async {
  sender.sendAny(sendableObject)
  sender.sendAny(nonSendableObject)
  // expected-warning@-1 {{conformance of 'NonSendableClass' to 'Sendable' is unavailable}}

  sender.sendOptionalAny(sendableObject)
  sender.sendOptionalAny(nonSendableObject)
  // expected-warning@-1 {{conformance of 'NonSendableClass' to 'Sendable' is unavailable}}

  sender.sendSendable(sendableObject)

  sender.sendSendableSubclasses(nonSendableObject)
  // expected-warning@-1 {{conformance of 'NonSendableClass' to 'Sendable' is unavailable}}
  sender.sendSendableSubclasses(sendableSubclassOfNonSendableObject)
  // expected-warning@-1 {{conformance of 'NonSendableClass' to 'Sendable' is unavailable}}
  // FIXME(rdar://89992569): Should allow for the possibility that NonSendableClass will have a Sendable subclass

  sender.sendProto(sendableProtos)
  sender.sendProto(nonSendableProtos)
  // expected-warning@-1 {{type 'any LabellyProtocol & ObjCClub' does not conform to the 'Sendable' protocol}}

  sender.sendProtos(sendableProtos)
  sender.sendProtos(nonSendableProtos)
  // expected-warning@-1 {{type 'any LabellyProtocol & ObjCClub' does not conform to the 'Sendable' protocol}}

  sender.sendAnyArray([sendableObject])
  sender.sendAnyArray([nonSendableObject])
  // expected-warning@-1 {{conformance of 'NonSendableClass' to 'Sendable' is unavailable; this is an error in Swift 6}}

  sender.sendGeneric(sendableGeneric)
  // expected-warning@-1{{type 'GenericObject<SendableClass>' does not conform to the 'Sendable' protocol}}
  // FIXME: Shouldn't warn

  sender.sendGeneric(nonSendableGeneric)
  // expected-warning@-1 {{type 'GenericObject<SendableClass>' does not conform to the 'Sendable' protocol}}

  sender.sendPtr(ptr)
  sender.sendStringArray(stringArray)
}

// Sendable checking
public struct SomeWrapper<T: AuditedNonSendable> {
  public let unit: T
}

extension SomeWrapper: Sendable where T: Sendable {}


// rdar://96830159
@available(SwiftStdlib 5.5, *)
@MainActor class SendableCompletionHandler {
  var isolatedThing: [String] = []
  // expected-note@-1 {{property declared here}}

  func makeCall(slowServer: SlowServer) {
    slowServer.doSomethingSlow("churn butter") { (_ : Int) in
      let _ = self.isolatedThing
      // expected-warning@-1 {{main actor-isolated property 'isolatedThing' can not be referenced from a Sendable closure; this is an error in Swift 6}}
    }
  }
}

// rdar://97646309 -- lookup and direct call of an optional global-actor constrained method would crash in SILGen
@available(SwiftStdlib 5.5, *)
extension CoffeeDelegate  {
    @MainActor func test() async -> (NSObject?, NSObject, NSObject) {
        return await self.icedMochaServiceGenerateMocha!(NSObject())
    }
}
