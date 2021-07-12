# Swift Concurrency Roadmap

Our goal is to make concurrent programming in Swift convenient, efficient, and safe. 

This document outlines a number of proposed additions and changes to the language to achieve this, through the implementation of asynchronous functions and actors. These additions will be proposed separately, but they will in many cases depend on each other. This document serves to join them together. Unlike a manifesto, which might describe multiple possible directions and in some cases unlikely directions, this document describes a single intended plan for tackling concurrency in Swift.

The end state of these changes will:

- make asynchronous programming convenient and clear at the point of use,
- provide a standard set of language tools and techniques that Swift developers can follow,
- improve the performance of asynchronous code through better knowledge at compile time, and
- eliminate data races and deadlocks in the same way Swift eliminates memory unsafety.

The introduction of these features will span multiple Swift releases. Features will be introduced in broadly two phases. The first phase introduces the `async` syntax and actor types; this will allow users to organize their code around actors in a way that will reduce, but not eliminate, data races. The second phase will enforce full actor isolation, eliminating data races, along with number of features to allow efficient and ergonomic interoperation of actors needed to make that isolation practical.

As a roadmap, this document does not go into the same level of detail as will those proposals. It also discusses features for the second phase, but detailed proposals for this area will wait until after the first phase is better defined.

There are a number of other related topics not covered in this document, such as asynchronous streams, parallel `for` loops, and distributed actors. Many of these features complement what is described in this roadmap and may be introduced at any point.

## Motivating Example

The basic patterns that we encourage for concurrency today are good: we tell people to protect their data with queues instead of locks, and to return the results of slow operations with asynchronous callbacks instead of blocking a thread. 

But doing these things manually is awful and error-prone. Consider the following snippet of code that demonstrates these patterns:

```swift
internal func refreshPlayers(completion: (() -> Void)? = nil) {
    refreshQueue.async {
        self.gameSession.allPlayers { players in
            self.players = players.map(\.nickname)
            completion?()
        }
    }
}
```

There are 3 observations we can make about this code:

- There’s **too much ceremony**. This function is fundamentally just calling a function, transforming the result, and assigning it to a property. But there’s so much extra work involved with the queue and the completion handlers that it’s hard to see that.

- This extra ceremony makes it **easy to introduce bugs**. There is a direct assignment to the `self.players` property in the completion handler. What thread is that on? It is unclear. This is a potential data race: the callback may need to dispatch back to the right queue before it does the assignment. Maybe this is handled by `allPlayers`, but we cannot reason locally about whether this code is thread-safe.

- This code is **needlessly inefficient**.  Several function objects need to be separately allocated.  References like `self` which are used by these functions must be copied into them, requiring extra reference-count operations. The functions may run multiple times or not at all, often preventing the compiler from avoiding these copies.

Furthermore, these problems are inescapably tied together.  Asynchronous callbacks are always eventually run exactly once, which means they cannot participate in a permanent reference cycle.  Since Swift doesn't know this, it requires `self` to be explicit in the closures.  Some programmers respond to this by reflexively adding `[weak self]`, increasing both the runtime overhead and the ceremony of the callback, since it must now handle the possibility of `self` being `nil`.  Frequently such functions immediately return when `self` is `nil`, making it harder to reason about their correctness since arbitrary amounts of code may have been skipped.

So the patterns shown here are good, but expressing them in Swift loses important structure and creates problems. The solution is to bring those patterns into the language. That will reduce boilerplate and let the language make the patterns safe, eliminating bugs and giving programmers the confidence to use concurrency more pervasively. It will also give us a chance to improve the performance of concurrent code.

Here is the above code rewritten using our proposed new syntax:

```swift
internal func refreshPlayers() async {
  players = await gameSession.allPlayers().map(\.nickname)
}
```

Things to note about this example:

- `refreshPlayers` is now an `async` function.
- `allPlayers` is also an `async` function, and _returns_ its result instead of passing it into a completion handler.
- Because of this, we can use expression composition to call the `map` function directly on the returned value.
- The `await` keyword appears before the expression calling `allPlayers`, indicating that the `refreshPlayers` function can become _suspended_ at this point.
- `await` works similarly to `try`, in that it need only appear once at the start of an expression that can suspend, not directly in front of every call that can suspend within that expression.
- The explicit `self.` has been eliminated from property accesses, because no escaping closures capturing `self` are required.
- The accesses to the properties `allPlayers` and `players` now cannot have data races.

To understand how the the last point is achieved we must step out a layer and look at how queues are used to protect state.

The original code was a method on a class that used `refreshQueue` to protect its internal state:

```swift
class PlayerRefreshController {
  var players: [String] = []
  var gameSession: GameSession
  var refreshQueue = DispatchQueue(label: "PlayerRefresh")
    
  func refreshPlayers(completion: (() -> Void)? = nil) { 
    ... 
  }
}
```

This is a common pattern: a class with a private queue and some properties that should only be accessed on the queue. We replace this manual queue management with an actor class:

```swift
actor class PlayerRefreshController {
  var players: [String] = []
  var gameSession: GameSession

  func refreshPlayers() async { ... }
}
```

Things to note about this example:

- Declaring a class to be an actor is similar to giving a class a private queue and synchronizing all access to its private state through that queue.
- Because this synchronization is now understood by the compiler, you cannot _forget_ to use the queue to protect state: the compiler will ensure that you are running on the queue in the class's methods, and it will prevent you from accessing the state outside those methods.
- Because the compiler is responsible for doing this, it can be smarter about optimizing away synchronization, like when a method starts by calling an async function on a different actor.

Having this static relationship between the actor and its functions and properties lets us enforce the isolation of data to an actor and define away data races. We statically know whether we’re in a context that can safely access an actor’s properties, and if not, the compiler handles switching into that context.

Above we've shown an actor class, where you’ve got a tightly-encapsulated set of properties and code. But the way we do UI programming today often spreads code across a large number of classes that you’re supposed to use from a single main thread. That main thread is still a kind of actor — it’s what we call a global actor.

You can mark classes and functions as being tied to that actor with an attribute. The compiler will let you reference this class from anywhere, but to actually call this method, you need to be on the UI actor. So, if it was appropriate for all the actions of `PlayerRefreshController` to be performed on the global UI actor, we would represent it like this:

```swift
@UIActor
class PlayerRefreshController {
  var players: [String] = []
  var gameSession: GameSession
    
  func refreshPlayers() async {  ...  }
}
```

## Proposals for the First Phase

We will be pitching the following proposals in the coming weeks in support of the first phase:

- **`async`/`await`** introduces a [coroutine-based](https://en.wikipedia.org/wiki/Coroutine) `async`/`await` model to Swift. Functions will opt into to being `async`, and can then `await` the results of other `async` functions, allowing asynchronous code to be expressed in a more natural "straight-line" form. Pitch [here](https://forums.swift.org/t/concurrency-asynchronous-functions/41619), proposal [here](https://github.com/DougGregor/swift-evolution/blob/async-await/proposals/nnnn-async-await.md).

- **`Task` API and Structured Concurrency** introduces the concept of a task to the standard library. This will cover APIs to create detached tasks, task "nurseries" for dynamically creating child tasks,  and the mechanics for cancellation and prioritization of tasks. It also introduces scope-based mechanisms for awaiting values from multiple child tasks, based on the principles of [structured concurrency](https://en.wikipedia.org/wiki/Structured_concurrency). Pitch [here](https://forums.swift.org/t/concurrency-structured-concurrency/41622), proposal [here](https://github.com/DougGregor/swift-evolution/blob/structured-concurrency/proposals/nnnn-structured-concurrency.md).

- **Actors & Actor Isolation** describes the actor model, which provides state isolation for concurrent programs. This provides the foundation for actor isolation, the mechanism through which potential for data races will be eliminated. This first phase proposal will introduce partial actor isolation, leaving full isolation enforcement to a subsequent proposal. Pitch [here](https://forums.swift.org/t/concurrency-actors-actor-isolation/41613), proposal [here](https://github.com/DougGregor/swift-evolution/blob/actors/proposals/nnnn-actors.md).

- **Concurrency Interoperability with Objective-C** introduces automated bridging between Swift's concurrency features (e.g., `async` functions) and the convention-based expression of asynchronous functions in Objective-C. This will allow existing asynchronous Objective-C APIs to be immediately usable with Swift's concurrency model by providing an alternate Swift translation of the API into an `async` function, alongside the callback-based version. Pitch [here](https://forums.swift.org/t/concurrency-interoperability-with-objective-c/41616), proposal [here](https://github.com/DougGregor/swift-evolution/blob/concurrency-objc/proposals/NNNN-concurrency-objc.md).

- **Async handlers** introduces the ability to declare a synchronous actor function as an asynchronous handler. These functions behave externally like a synchronous function, but internally are handled like an asynchronous function. This allows traditional “notification” methods, such as those on `UITableViewDelegate`, to perform asynchronous operations without cumbersome setup.

## Actor Isolation and The Second Phase

The goal is for Swift to prevent data races on mutable state by default. The system through which this is achieved is called **actor isolation**, both because actors are centrally important to how the system works and because the system is centrally concerned with preventing actor-protected state from being accessed outside the actor. However, actor isolation restricts code even when actors aren’t directly involved, when it is necessary for the correctness of the system under concurrency.

We intend to introduce the features described in this roadmap in two phases: first by introducing the ability to create async functions and actors; and second, by enforcing full actor isolation.

The basic idea of actor isolation is similar to the idea of [exclusive access to memory](https://github.com/apple/swift-evolution/blob/main/proposals/0176-enforce-exclusive-access-to-memory.md), and builds upon it. Swift’s concurrency design aims to provide an easy-to-use and composable approach to safe concurrency by starting from the natural isolation of actors and then using ownership as a complementary tool.

The problem of actor isolation reduces to the problem of ensuring that all ordinary mutable memory is accessed only by a particular actor or task.  This in turn reduces to an analysis of how the memory is accessed and who can access it in the first place. We can categorize memory into a few groups:

* Properties of an actor will be protected by the actor.
* Immutable memory (such as a `let` constant), local memory (such as a local variable that’s never captured), and value component memory (such as a properties of a struct, or an enum case), are already protected from data races.
* Unsafe memory (such as an arbitrary allocation referenced by an `UnsafeMutablePointer`) is associated with an unsafe abstraction.  It’s actively undesirable to try to force these abstractions to be used safely, because these abstractions are meant to be usable to bypass safe language rules when necessary. Instead, we have to trust the programmer to use these correctly.
* Global memory (such as a global or static variable) can in principle be accessed by any code anywhere, so is subject to data races.
* Class component memory can also be accessed from any code that hold a reference to the class. This means that while the _reference_ to the class may be protected by an actor, passing that reference between actors exposes its properties to data races. This also includes references to classes held within value types, when these are passed between actors.

The goal of **full actor isolation** is to ensure that these last two categorizations are protected by default.

### First Phase: Basic Actor Isolation

The first phase introduces safety benefits. Users will be able to protect  global variables with global actors, and to protect class members by converting them to actor classes. Frameworks that require access on a particular queue can define a global actor and default protocols to it.

A number of important cases of actor isolation will be enforced in this stage:

```swift
actor class MyActor {
  let immutable: String = "42"
  var mutableArray: [String] = []

  func synchronousFunction() {
    mutableArray += ["syncFunction called"]
  }
}

extension MyActor {

  func asyncFunction(other: MyActor) async {
    // allowed: an actor can access its internal state, even in an extension
    self.mutableArray += ["asyncFunction called"]
  
    // allowed: immutable memory can be accessed from outside the actor
    print(other.immutable)

    // error: an actor cannot access another's mutable state
    otherActor.mutableArray += ["not allowed"]

    // error: either reading or writing
    print(other.mutableArray.first)
    
    // allowed: async functions can call async functions on other actors
    await other.asyncFunction(otherActor: self)
    
    // error: only asynchronous functions can be called from outside the actor
    other.synchronousFunction()    
  }
}
```

These enforcements are *non source breaking* because actors and async functions are a new feature.

### Second Phase: Full Actor Isolation

Even after the introduction of actors, there will still exist the possibility for race conditions, through global variables and values of reference type:

```swift
var racyGlobal: [String] = []

@MyGlobalActor
var safeGlobal: [String] = []

class PlainOldClass {
  var unprotectedState: String = []
}

actor class RacyActor {
  let immutableClassReference: PlainOldClass

  func racyFunction(other: RacyActor) async {
    // protected: global variable protected by a global actor
    safeGlobal += ["Safe access"]
  
    // unprotected: global variable not in an actor
    racyGlobal += ["Racy access"]
    
    // unprotected: racyProperty is immutable, but it is a reference type
    // so it allows access to unprotected shared mutable type
    other.takeClass(immutableClassReference)
  }
  
  func takeClass(_ plainClass: PlainOldClass) {
    plainClass.unprotectedState += ["Racy access"]  
  }
}
```

In the first phase, we intend to preserve Swift's *current* default behavior: global variables and class component memory are not protected from data races. "Actor unsafe" is therefore the default for this memory. Because this is the default of current Swift, enabling this first phase is *non-source breaking*.

In the second phase, the introduction of further features will provide a full set of tools for working with fully isolated actors. The most important of these is the ability to restrict a type to be "actor local". When a type is marked actor local, the compiler will prevent it from being passed between actors. Instead, the reference would need to be somehow cloned/unshared before being passed across a boundary.

This in turn will allow the defaults to be changed:

- Global variables will be required to be protected by a global actor, or marked "actor unsafe".
- Classes (and types that contain class references) will change from being from being "actor unsafe" by default to being "actor local".

This change in default will necessitate a **source break**, and will need to be gated by a language mode. Code that touches a mutable global variable or shares a class reference across actor boundaries fundamentally cannot be shown to be safe from data races, and will need to change to ensure that it (and code written in the future) is safe from data races. It is hoped that this source break will not be onerous:

- it is expected that use of global variables should be sparing, and most global variables can be protected by a global actor;
- as long as classes are not shared across actor boundaries, "actor local" annotation should  not affect code within an actor;
- where references must be passed across boundaries, the language should make this obvious and the solutions simple;
- by further encouraging and simplifying the use of value types, the need to share classes across actor boundaries should be reduced;
- the time between the two phases will give users an opportunity to factor their code into actors and async functions, preparing it for full isolation.

Unlike the pitches for the first phase, which will be pitched and proposed, the language features needed for the second phase will initially be raised for _discussion_ on the evolution discussion section of the Swift forums. One of the main drivers for the two-phase approach is that of wanting to give Swift users time to become accustomed to async functions and actors before moving to a full isolation model. It is expected that the experience of porting large working code bases to actors and async functions will inform the functionality needed to enforce full actor isolation. This feedback should inform the discussion of features in the second phase.

Features expected to be discussed as part of a second phase include:

- introduction of an `actorlocal` restriction on types;
- compiler support for guaranteed correct "copy on write" types via a `mutableIfUnique` class type;
- attributes to opt-out of actor isolation, for example where thread safety is being handled through some other means.

## Glossary of Concepts

These are the basic concepts that will be used throughout the design, briefly defined here.

* A **synchronous function** is the kind of function that Swift programmers are already used to: it runs to completion on a single thread, with no interleaving code except for any synchronous functions it calls.
* A **thread** refers to the underlying platform’s concept of threads. Platforms vary, but tend to share basic characteristics: true concurrency requires creating a platform thread, but creating and running platform threads is expensive. C function calls, and ordinary synchronous Swift functions, require the use of a platform thread.
* An **asynchronous function** is a new kind of function which does not have to run to completion without interruption. An interruption causes the function to be **suspended**. The point at which an async function may give up its thread is a **suspension point**.
* A **task** is an operation that runs asynchronously. All asynchronous functions run as part of some task.  When an asynchronous function calls another asynchronous function, the call is still part of the the same task, even if the call has to change actors. A task is the analogue of a thread for asynchronous functions.
* An async function can create a **child task**. Child tasks inherit some of the structure of their parent task, including its priority, but can run concurrently with it.  However, this concurrency is bounded: a function that creates a child task must wait for it to end before returning.
* A program wishes to initiate independent concurrent work that can outlast its spawning context uses a **detached task** rather than a bounded child task.
* A **partial task** is a unit of schedulable work. When the currently-executing function in a task is suspended, that is the end of the partial task, and a new partial task is created to continue the overall task’s work.
* An **executor** is a service which accepts the submission of partial tasks and arranges for some thread to run them. An async function that is currently running always knows the executor on which it's running. An executor is called **exclusive** if the partial tasks submitted to it will never be run concurrently.
* An **actor** is an independent part of the program which can run code.  It can only run one piece of code at a time — that is, it acts as an exclusive executor — but the code it runs can execute concurrently with the code run by other actors.
* An actor can have protected state which can only be accessed by that actor. The system through which this is achieved is called **actor isolation**. The long term goal is for Swift to guarantee actor isolation by default.
* An **actor class** is a reference type, each instance of which is a separate actor. Its protected state is its instance properties, and its actor functions are its instance methods.
* A **global actor** is a global object. Its protected state and actor functions may be spread across many different types.  They can be marked with an actor-specific attribute, which Swift can infer in many cases.
