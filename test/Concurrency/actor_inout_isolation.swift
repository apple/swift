// RUN: %target-swift-frontend  -disable-availability-checking %s -emit-sil -o /dev/null -verify -verify-additional-prefix minimal-
// RUN: %target-swift-frontend  -disable-availability-checking %s -emit-sil -o /dev/null -verify -verify-additional-prefix targeted-complete-tns- -verify-additional-prefix complete-tns- -strict-concurrency=complete

// REQUIRES: concurrency
// REQUIRES: asserts

// Verify that we don't allow actor-isolated state to be passed via inout
// Check:
//  - can't pass it into a normal async function
//  - can't pass it into a first-class async function as a value
//  - can't pass it into another actor method
//  - can't pass it into a curried/partially applied function
//  - can't pass it inout to a function that doesn't directly touch it
//  - can't pass it into a function that was passed into the calling method
//  - can't call async mutating function on actor isolated state

struct Point {
  var x: Int
  var y: Int
  var z: Int? = nil

  mutating func setComponents(x: inout Int, y: inout Int) async {
    defer { (x, y) = (self.x, self.y) }
    (self.x, self.y) = (x, y)
  }
}

@available(SwiftStdlib 5.1, *)
actor TestActor {
  // expected-note@+1{{mutation of this property is only permitted within the actor}}
  var position = Point(x: 0, y: 0) // expected-note 2{{property declared here}}
  var nextPosition = Point(x: 0, y: 1) // expected-note 2{{property declared here}}
  var value1: Int = 0 // expected-note 6{{property declared here}}
  var value2: Int = 1 // expected-note 4{{property declared here}}
  var points: [Point] = [] // expected-note {{property declared here}}

  subscript(x : inout Int) -> Int { // expected-error {{'inout' may only be used on function or initializer parameters}}
    x += 1
    return x
  }
}

@available(SwiftStdlib 5.1, *)
@Sendable func modifyAsynchronously(_ foo: inout Int) async { foo += 1 }
@available(SwiftStdlib 5.1, *)
enum Container {
  static let modifyAsyncValue = modifyAsynchronously
}

// external function call
@available(SwiftStdlib 5.1, *)
extension TestActor {

  // Can't pass actor-isolated primitive into a function
  func inoutAsyncFunctionCall() async {
    // expected-error@+1{{actor-isolated property 'value1' cannot be passed 'inout' to 'async' function call}}
    await modifyAsynchronously(&value1)
  }

  func inoutAsyncClosureCall() async {
    // expected-error@+1{{actor-isolated property 'value1' cannot be passed 'inout' to 'async' function call}}
    await { (_ foo: inout Int) async in foo += 1 }(&value1)
  }

  // Can't pass actor-isolated primitive into first-class function value
  func inoutAsyncValueCall() async {
    // expected-error@+1{{actor-isolated property 'value1' cannot be passed 'inout' to 'async' function call}}
    await Container.modifyAsyncValue(&value1)
  }

  // Can't pass property of actor-isolated state inout to async function
  func inoutPropertyStateValueCall() async {
    // expected-error@+1{{actor-isolated property 'position' cannot be passed 'inout' to 'async' function call}}
    await modifyAsynchronously(&position.x)
  }

  func nestedExprs() async {
    // expected-error@+1{{actor-isolated property 'position' cannot be passed 'inout' to 'async' function call}}
    await modifyAsynchronously(&position.z!)

    // expected-error@+1{{actor-isolated property 'points' cannot be passed 'inout' to 'async' function call}}
    await modifyAsynchronously(&points[0].z!)
  }

}

// internal method call
@available(SwiftStdlib 5.1, *)
extension TestActor {
  func modifyByValue(_ other: inout Int) async {
    other += value1
  }

  func passStateIntoMethod() async {
    // expected-error@+1{{actor-isolated property 'value1' cannot be passed 'inout' to 'async' function call}}
    await modifyByValue(&value1)
  }
}

// external class method call
@available(SwiftStdlib 5.1, *)
class NonAsyncClass {
  // expected-targeted-complete-tns-note @-1 {{class 'NonAsyncClass' does not conform to the 'Sendable' protocol}}
  // expected-tns-note @-2 {{class 'NonAsyncClass' does not conform to the 'Sendable' protocol}}
  func modifyOtherAsync(_ other : inout Int) async {
    // ...
  }

  func modifyOtherNotAsync(_ other: inout Int) {
    // ...
  }
}

// Calling external class/struct async function
@available(SwiftStdlib 5.1, *)
extension TestActor {
  // Can't pass state into async method of another class

  func passStateIntoDifferentClassMethod() async {
    let other = NonAsyncClass()
    let otherCurry = other.modifyOtherAsync
    // expected-targeted-complete-tns-warning @-1 {{non-sendable type 'NonAsyncClass' exiting actor-isolated context in call to non-isolated instance method 'modifyOtherAsync' cannot cross actor boundary}}
    await other.modifyOtherAsync(&value2)
    // expected-error @-1 {{actor-isolated property 'value2' cannot be passed 'inout' to 'async' function call}}

    await otherCurry(&value1)
    // expected-error @-1 {{actor-isolated property 'value1' cannot be passed 'inout' to 'async' function call}}

    other.modifyOtherNotAsync(&value2) // This is okay since it's not async!

  }

  func callMutatingFunctionOnStruct() async {
    // expected-error@+3:20{{cannot call mutating async function 'setComponents(x:y:)' on actor-isolated property 'position'}}
    // expected-error@+2:38{{actor-isolated property 'nextPosition' cannot be passed 'inout' to 'async' function call}}
    // expected-error@+1:58{{actor-isolated property 'nextPosition' cannot be passed 'inout' to 'async' function call}}
    await position.setComponents(x: &nextPosition.x, y: &nextPosition.y)

    // expected-error@+3:20{{cannot call mutating async function 'setComponents(x:y:)' on actor-isolated property 'position'}}
    // expected-error@+2:38{{actor-isolated property 'value1' cannot be passed 'inout' to 'async' function call}}
    // expected-error@+1:50{{actor-isolated property 'value2' cannot be passed 'inout' to 'async' function call}}
    await position.setComponents(x: &value1, y: &value2)
  }
}

// Check implicit async testing
@available(SwiftStdlib 5.1, *)
actor DifferentActor {
  func modify(_ state: inout Int) {}
}

@available(SwiftStdlib 5.1, *)
extension TestActor {
  func modify(_ state: inout Int) {}

  // Actor state passed inout to implicitly async function on an actor of the
  // same type
  func modifiedByOtherTestActor(_ other: TestActor) async {
    //expected-error@+1{{actor-isolated property 'value2' cannot be passed 'inout' to implicitly 'async' function call}}
    await other.modify(&value2)
  }

  // Actor state passed inout to an implicitly async function on an actor of a
  // different type
  func modifiedByOther(_ other: DifferentActor) async {
    //expected-error@+1{{actor-isolated property 'value2' cannot be passed 'inout' to implicitly 'async' function call}}
    await other.modify(&value2)
  }
}

@available(SwiftStdlib 5.1, *)
actor MyActor {
  var points: [Point] = [] // expected-note 2{{property declared here}}
  var int: Int = 0 // expected-note 2{{property declared here}}
  var maybeInt: Int? // expected-note 1{{property declared here}}
  var maybePoint: Point? // expected-note 1{{property declared here}}
  var myActor: TestActor = TestActor() // expected-note 1{{property declared here}}

  // Checking that various ways of unwrapping emit the right error messages at
  // the right times and that illegal operations are caught
  func modifyStuff() async {
    // expected-error@+1{{actor-isolated property 'points' cannot be passed 'inout' to 'async' function call}}
    await modifyAsynchronously(&points[0].x)
    // expected-error@+1{{actor-isolated property 'points' cannot be passed 'inout' to 'async' function call}}
    await modifyAsynchronously(&points[0].z!)
    // expected-error@+1{{actor-isolated property 'int' cannot be passed 'inout' to 'async' function call}}
    await modifyAsynchronously(&int)
    // expected-error@+1{{actor-isolated property 'maybeInt' cannot be passed 'inout' to 'async' function call}}
    await modifyAsynchronously(&maybeInt!)
    // expected-error@+1{{actor-isolated property 'maybePoint' cannot be passed 'inout' to 'async' function call}}
    await modifyAsynchronously(&maybePoint!.z!)
    // expected-error@+1{{actor-isolated property 'int' cannot be passed 'inout' to 'async' function call}}
    await modifyAsynchronously(&(int))

    // expected-error@+1{{cannot pass immutable value of type 'Int' as inout argument}}
    await modifyAsynchronously(&(maybePoint?.z)!)
    // expected-error@+2{{actor-isolated property 'position' can not be used 'inout' on a non-isolated actor instance}}
    // expected-error@+1{{actor-isolated property 'myActor' cannot be passed 'inout' to 'async' function call}}
    await modifyAsynchronously(&myActor.position.x)
  }
}

// Verify global actor protection

@available(SwiftStdlib 5.1, *)
@globalActor
struct MyGlobalActor {
  static let shared = TestActor()
}

@MyGlobalActor var number: Int = 0
// expected-note @-1 {{var declared here}}
// expected-note @-2 {{var declared here}}
// expected-note @-3 {{mutation of this var is only permitted within the actor}}
// expected-complete-tns-error @-4 {{top-level code variables cannot have a global actor}}
// expected-complete-tns-note @-5 4{{mutation of this var is only permitted within the actor}}


if #available(SwiftStdlib 5.1, *) {
  let _ = Task.detached { await { (_ foo: inout Int) async in foo += 1 }(&number) }
  // expected-error @-1 {{actor-isolated var 'number' cannot be passed 'inout' to 'async' function call}}
  // expected-minimal-error @-2 {{global actor 'MyGlobalActor'-isolated var 'number' can not be used 'inout' from a non-isolated context}}
  // expected-complete-tns-error @-3 {{main actor-isolated var 'number' can not be used 'inout' from a non-isolated context}}
}

// attempt to pass global state owned by the global actor to another async function
@available(SwiftStdlib 5.1, *)
@MyGlobalActor func sneaky() async { await modifyAsynchronously(&number) }
// expected-error @-1 {{actor-isolated var 'number' cannot be passed 'inout' to 'async' function call}}
// expected-complete-tns-error @-2 {{main actor-isolated var 'number' can not be used 'inout' from global actor 'MyGlobalActor'}}


// It's okay to pass actor state inout to synchronous functions!

func globalSyncFunction(_ foo: inout Int) { }
@available(SwiftStdlib 5.1, *)
@MyGlobalActor func globalActorSyncFunction(_ foo: inout Int) { }
@available(SwiftStdlib 5.1, *)
@MyGlobalActor func globalActorAsyncOkay() async { globalActorSyncFunction(&number) }
// expected-complete-tns-error @-1 {{main actor-isolated var 'number' can not be used 'inout' from global actor 'MyGlobalActor'}}
@available(SwiftStdlib 5.1, *)
@MyGlobalActor func globalActorAsyncOkay2() async { globalSyncFunction(&number) }
// expected-complete-tns-error @-1 {{main actor-isolated var 'number' can not be used 'inout' from global actor 'MyGlobalActor'}}
@available(SwiftStdlib 5.1, *)
@MyGlobalActor func globalActorSyncOkay() { globalSyncFunction(&number) }
// expected-complete-tns-error @-1 {{main actor-isolated var 'number' can not be used 'inout' from global actor 'MyGlobalActor'}}

// Gently unwrap things that are fine
@available(SwiftStdlib 5.1, *)
struct Cat {
  mutating func meow() async { }
}

@available(SwiftStdlib 5.1, *)
struct Dog {
  var cat: Cat?

  mutating func woof() async {
    // This used to cause the compiler to crash, but should be fine
    await cat?.meow()
  }
}

@available(SwiftStdlib 5.1, *)
func passToAsync(_: Int) async {}

@available(SwiftStdlib 5.1, *)
func wrapInClosure(
  @_inheritActorContext _ block: @Sendable () async throws -> Void
) async {}

@available(SwiftStdlib 5.1, *)
extension Array {
  var mutateAsynchronously: Int {
    mutating get async { 0 }
  }

  subscript(mutateAsynchronously i: Int) -> Int {
    mutating get async { 0 }
  }
}

@available(SwiftStdlib 5.1, *)
actor ProtectArray {
  var array: [Int] = []
  // expected-note@-1 {{property declared here}}

  func test() async {
    // FIXME: this is invalid too!
    _ = await array.mutateAsynchronously
    // expected-targeted-complete-tns-warning@-1 {{non-sendable type '@lvalue [Int]' exiting actor-isolated context in call to non-isolated property 'mutateAsynchronously' cannot cross actor boundary}}

    _ = await array[mutateAsynchronously: 0]
    // expected-error@-1 {{actor-isolated property 'array' cannot be passed 'inout' to 'async' function call}}
    // expected-targeted-complete-tns-warning@-2 {{non-sendable type 'inout Array<Int>' exiting actor-isolated context in call to non-isolated subscript 'subscript(mutateAsynchronously:)' cannot cross actor boundary}}

    await passToAsync(array[0])

    await wrapInClosure {
      _ = array[0]
      array.append(1)
    }
  }
}

extension Optional {
  mutating func mutate() async {}
}

@available(SwiftStdlib 5.1, *)
actor ProtectDictionary {
  var dict: [Int: Int] = [:]

  func invalid() async {
    await dict[0].mutate()
    // expected-warning@-1 {{cannot call mutating async function 'mutate()' on actor-isolated property 'dict'; this is an error in the Swift 6 language mode}}
  }
}
