// RUN: %target-swift-frontend -strict-concurrency=complete -swift-version 5 -parse-as-library -emit-sil -verify %s
// RUN: %target-swift-frontend -strict-concurrency=complete -swift-version 5 -parse-as-library -emit-sil -verify %s -enable-experimental-feature RegionBasedIsolation

// REQUIRES: asserts

func randomBool() -> Bool { return false }
func logTransaction(_ i: Int) {}

enum Color: Error {
  case red
  case yellow
  case blue
}

func takeNonSendable(_ ns: NonSendableType) {}

@available(SwiftStdlib 5.1, *)
func takeSendable(_ s: SendableType) {}

class NonSendableType {
  var x: Int = 0
  func f() {}
}

@available(SwiftStdlib 5.1, *)
struct SendableType: Sendable {}

struct Money {
  var dollars: Int
  var euros: Int {
    return dollars * 2
  }
}

@available(SwiftStdlib 5.1, *)
func takeBob(_ b: Bob) {}

@available(SwiftStdlib 5.1, *)
actor Bob {
  var x: Int

  nonisolated func speak() { }
  nonisolated var cherry : String {
        get { "black cherry" }
    }

  init(v0 initial: Int) {
    self.x = 0
    speak()    // expected-note {{after calling instance method 'speak()', only non-isolated properties of 'self' can be accessed from this init}}
    speak()
    self.x = 1 // expected-warning {{cannot access property 'x' here in non-isolated initializer; this is an error in Swift 6}}
    speak()
  }

  init(v1 _: Void) {
    self.x = 0
    _ = cherry  // expected-note {{after accessing property 'cherry', only non-isolated properties of 'self' can be accessed from this init}}
    self.x = 1  // expected-warning {{cannot access property 'x' here in non-isolated initializer; this is an error in Swift 6}}
  }

  init(v2 callBack: (Bob) -> Void) {
    self.x = 0
    callBack(self)  // expected-note {{after a call involving 'self', only non-isolated properties of 'self' can be accessed from this init}}
    self.x = 1      // expected-warning {{cannot access property 'x' here in non-isolated initializer; this is an error in Swift 6}}
  }

  init(v3 _: Void) {
    self.x = 1
    takeBob(self)   // expected-note {{after calling global function 'takeBob', only non-isolated properties of 'self' can be accessed from this init}}
    self.x = 1  // expected-warning {{cannot access property 'x' here in non-isolated initializer; this is an error in Swift 6}}
  }



  // ensure noniso does not flow out of a defer's unreachable block
  init(spice doesNotFlow: Int) {
    if true {
      defer {
        if randomBool() {
          speak()
          fatalError("oops")
        }
      }
      self.x = 0
    }
    self.x = 20
  }
}

@available(SwiftStdlib 5.1, *)
actor Casey {
  var money: Money

  nonisolated func speak(_ msg: String) { print("Casey: \(msg)") }
  nonisolated func cashUnderMattress() -> Int { return 0 }

  init() {
    money = Money(dollars: 100)
    defer { logTransaction(money.euros) } // expected-warning {{cannot access property 'money' here in non-isolated initializer; this is an error in Swift 6}}
    self.speak("Yay, I have $\(money.dollars)!") // expected-note {{after calling instance method 'speak', only non-isolated properties of 'self' can be accessed from this init}}
  }

  init(with start: Int) {
    money = Money(dollars: start)

    if (money.dollars < 0) {
      self.speak("Oh no, I'm in debt!") // expected-note 3 {{after calling instance method 'speak', only non-isolated properties of 'self' can be accessed from this init}}
    }
    logTransaction(money.euros) // expected-warning {{cannot access property 'money' here in non-isolated initializer; this is an error in Swift 6}}

    // expected-warning@+1 2 {{cannot access property 'money' here in non-isolated initializer; this is an error in Swift 6}}
    money.dollars = money.dollars + 1

    if randomBool() {
      // expected-note@+2 {{after calling instance method 'cashUnderMattress()', only non-isolated properties of 'self' can be accessed from this init}}
      // expected-warning@+1 {{cannot access property 'money' here in non-isolated initializer; this is an error in Swift 6}}
      money.dollars += cashUnderMattress()
    }
  }
}

@available(SwiftStdlib 5.1, *)
actor Demons {
  let ns: NonSendableType

  init(_ x: NonSendableType) {
    self.ns = x
  }

  nonisolated deinit {
    let _ = self.ns // expected-warning {{cannot access property 'ns' with a non-sendable type 'NonSendableType' from non-isolated deinit; this is an error in Swift 6}}
  }
}

func pass<T>(_ t: T) {}

@available(SwiftStdlib 5.1, *)
actor ExampleFromProposal {
  let immutableSendable = SendableType()
  var mutableSendable = SendableType()
  let nonSendable = NonSendableType()
  var nsItems: [NonSendableType] = []
  var sItems: [SendableType] = []

  init() {
    _ = self.immutableSendable  // ok
    _ = self.mutableSendable    // ok
    _ = self.nonSendable        // ok

    f() // expected-note 2 {{after calling instance method 'f()', only non-isolated properties of 'self' can be accessed from this init}}

    _ = self.immutableSendable  // ok
    _ = self.mutableSendable    // expected-warning {{cannot access property 'mutableSendable' here in non-isolated initializer; this is an error in Swift 6}}
    _ = self.nonSendable        // expected-warning {{cannot access property 'nonSendable' here in non-isolated initializer; this is an error in Swift 6}}
  }


  nonisolated deinit {
    _ = self.immutableSendable  // ok
    _ = self.mutableSendable    // ok
    _ = self.nonSendable        // expected-warning {{cannot access property 'nonSendable' with a non-sendable type 'NonSendableType' from non-isolated deinit; this is an error in Swift 6}}

    f() // expected-note {{after calling instance method 'f()', only non-isolated properties of 'self' can be accessed from a deinit}}

    _ = self.immutableSendable  // ok
    _ = self.mutableSendable    // expected-warning {{cannot access property 'mutableSendable' here in deinitializer; this is an error in Swift 6}}
    _ = self.nonSendable        // expected-warning {{cannot access property 'nonSendable' with a non-sendable type 'NonSendableType' from non-isolated deinit; this is an error in Swift 6}}
  }

  nonisolated func f() {}
}

@available(SwiftStdlib 5.1, *)
@MainActor
class CheckGAIT1 {
  var silly = 10
  var ns = NonSendableType()

  nonisolated func f() {}

  nonisolated init(_ c: Color) {
    silly += 0
    repeat {
      switch c {
        case .red:
          continue
        case .yellow:
          silly += 1
          continue
        case .blue:
          f() // expected-note {{after calling instance method 'f()', only non-isolated properties of 'self' can be accessed from this init}}
      }
      break
    } while true
    silly += 2 // expected-warning {{cannot access property 'silly' here in non-isolated initializer; this is an error in Swift 6}}
  }

  nonisolated deinit {
    _ = ns // expected-warning {{cannot access property 'ns' with a non-sendable type 'NonSendableType' from non-isolated deinit; this is an error in Swift 6}}
    f()     // expected-note {{after calling instance method 'f()', only non-isolated properties of 'self' can be accessed from a deinit}}
    _ = silly // expected-warning {{cannot access property 'silly' here in deinitializer; this is an error in Swift 6}}

  }
}

@available(SwiftStdlib 5.1, *)
actor ControlFlowTester {
  let ns: NonSendableType = NonSendableType()
  let s: SendableType = SendableType()
  var count: Int = 0

  nonisolated func noniso() {}

  init(v1: Void) {
    noniso()                 // expected-note {{after calling instance method 'noniso()', only non-isolated properties of 'self' can be accessed from this init}}
    takeNonSendable(self.ns) // expected-warning {{cannot access property 'ns' here in non-isolated initializer; this is an error in Swift 6}}
    takeSendable(self.s)
  }

  init(v2 c: Color) throws {
    if c == .red {
      noniso()
      throw c
    } else if c == .blue {
      noniso()
      fatalError("soul")
    }
    count += 1
  }

  init(v3 c: Color) {
    do {
      defer { noniso() } // expected-note 3 {{after calling instance method 'noniso()', only non-isolated properties of 'self' can be accessed from this init}}
      switch c {
      case .red:
        throw Color.red
      case .blue:
        throw Color.blue
      case .yellow:
        count = 0
        throw Color.yellow
      }
    } catch Color.blue {
      count += 1 // expected-warning {{cannot access property 'count' here in non-isolated initializer; this is an error in Swift 6}}
    } catch {
      count += 1 // expected-warning {{cannot access property 'count' here in non-isolated initializer; this is an error in Swift 6}}
    }
    count = 42 // expected-warning {{cannot access property 'count' here in non-isolated initializer; this is an error in Swift 6}}
  }


  init(v4 c: Color) {
    defer { count += 1 } // expected-warning {{cannot access property 'count' here in non-isolated initializer; this is an error in Swift 6}}
    noniso() // expected-note 1 {{after calling instance method 'noniso()', only non-isolated properties of 'self' can be accessed from this init}}
  }

  init?(v5 c: Color) throws {
    if c != .red {
      noniso()
      return nil
    }
    count += 1
    noniso()  // expected-note {{after calling instance method 'noniso()', only non-isolated properties of 'self' can be accessed from this init}}
    if c == .blue {
      throw c
    }
    count += 1 // expected-warning {{cannot access property 'count' here in non-isolated initializer; this is an error in Swift 6}}
  }

  init(v5 c: Color?) {
    if let c = c {
      switch c {
      case .red:
        defer { noniso() } // expected-note 5 {{after calling instance method 'noniso()', only non-isolated properties of 'self' can be accessed from this init}}
        count = 0 // OK
        fallthrough
      case .blue:
        count = 1 // expected-warning {{cannot access property 'count' here in non-isolated initializer; this is an error in Swift 6}}
        fallthrough
      case .yellow:
        count = 2 // expected-warning {{cannot access property 'count' here in non-isolated initializer; this is an error in Swift 6}}
        break
      }
    }
    switch c {
    case .some(.red):
      count += 1  // expected-warning {{cannot access property 'count' here in non-isolated initializer; this is an error in Swift 6}}
    case .some(.blue):
      count += 2  // expected-warning {{cannot access property 'count' here in non-isolated initializer; this is an error in Swift 6}}
    case .some(.yellow):
      count += 3  // expected-warning {{cannot access property 'count' here in non-isolated initializer; this is an error in Swift 6}}
    case .none:
      break
    }
  }
}

enum BogusError: Error {
    case blah
}

@available(SwiftStdlib 5.1, *)
actor Convenient {
    var x: Int
    var y: Convenient?

    init(val: Int) {
        self.x = val
    }

    init(bigVal: Int) {
        if bigVal < 0 {
            self.init(val: 0)
            say(msg: "hello from actor!")
        }
        say(msg: "said this too early!") // expected-error {{'self' used before 'self.init' call or assignment to 'self'}}
        self.init(val: bigVal)

        Task { await self.mutateIsolatedState() }
    }

    init!(biggerVal1 biggerVal: Int) {
        guard biggerVal < 1234567 else { return nil }
        self.init(bigVal: biggerVal)
        say(msg: "hello?")
    }

    @MainActor
    init?(biggerVal2 biggerVal: Int) async {
        guard biggerVal < 1234567 else { return nil }
        self.init(bigVal: biggerVal)
        say(msg: "hello?")
        await mutateIsolatedState()
    }

    init() async {
        self.init(val: 10)
        self.x += 1
        mutateIsolatedState()
    }

    init(throwyDesignated val: Int) throws {
        guard val > 0 else { throw BogusError.blah }
        self.x = 10
        say(msg: "hello?")

        Task { self }
    }

    init(asyncThrowyDesignated val: Int) async throws {
        guard val > 0 else { throw BogusError.blah }
        self.x = 10
        say(msg: "hello?")
        Task { self }
    }

    init(throwyConvenient val: Int) throws {
        try self.init(throwyDesignated: val)
        say(msg: "hello?")
        Task { self }
    }

    func mutateIsolatedState() {
        self.y = self
    }

    nonisolated func say(msg: String) {
        print(msg)
    }
}

func randomInt() -> Int { return 4 }

@available(SwiftStdlib 5.1, *)
func callMethod(_ a: MyActor) {}

@available(SwiftStdlib 5.1, *)
func passInout<T>(_ a: inout T) {}

@available(SwiftStdlib 5.1, *)
actor MyActor {
    var x: Int
    var y: Int
    var hax: MyActor?

    var computedProp : Int {
        get { 0 }
        set { }
    }

    func helloWorld() {}

    init(ci1 c: Bool) {
        self.init(i1: c)
        Task { self }
        callMethod(self)
    }

    init(ci2 c: Bool) async {
      self.init(i1: c)
      self.x = 1
      callMethod(self)
      self.x = 0
    }

    init(i1 c:  Bool) {
        self.x = 0
        _ = self.x
        self.y = self.x

        Task { self } // expected-note 2 {{after making a copy of 'self', only non-isolated properties of 'self' can be accessed from this init}}

        self.x = randomInt()  // expected-warning {{cannot access property 'x' here in non-isolated initializer; this is an error in Swift 6}}

        callMethod(self) // expected-note 5 {{after calling global function 'callMethod', only non-isolated properties of 'self' can be accessed from this init}}

        passInout(&self.x) // expected-warning {{cannot access property 'x' here in non-isolated initializer; this is an error in Swift 6}}

        if c {
          // expected-warning@+2 {{cannot access property 'y' here in non-isolated initializer; this is an error in Swift 6}}
          // expected-warning@+1 {{cannot access property 'x' here in non-isolated initializer; this is an error in Swift 6}}
          self.x = self.y
        }

        // expected-warning@+2 {{cannot access property 'y' here in non-isolated initializer; this is an error in Swift 6}}
        // expected-warning@+1 {{cannot access property 'x' here in non-isolated initializer; this is an error in Swift 6}}
        (_, _) = (self.x, self.y)
        _ = self.x == 0 // expected-warning {{cannot access property 'x' here in non-isolated initializer; this is an error in Swift 6}}

        while c {
          // expected-warning@+2 {{cannot access property 'hax' here in non-isolated initializer; this is an error in Swift 6}}
          // expected-note@+1 2 {{after making a copy of 'self', only non-isolated properties of 'self' can be accessed from this init}}
          self.hax = self
          _ = self.hax  // expected-warning {{cannot access property 'hax' here in non-isolated initializer; this is an error in Swift 6}}
        }

        Task {
            _ = await self.hax
            await self.helloWorld()
        }

        { _ = self }()
    }

    @MainActor
    init(i2 c:  Bool) {
        self.x = 0
        self.y = self.x

        Task { self } // expected-note {{after making a copy of 'self', only non-isolated properties of 'self' can be accessed from this init}}

        callMethod(self)

        passInout(&self.x) // expected-warning {{cannot access property 'x' here in non-isolated initializer; this is an error in Swift 6}}
    }

    init(i3 c:  Bool) async {
        self.x = 0
        self.y = self.x

        Task { self }

        self.helloWorld()
        callMethod(self)
        passInout(&self.x)

        self.x = self.y

        self.hax = self
        _ = Optional.some(self)

        _ = computedProp
        computedProp = 1

        Task {
            _ = self.hax
            self.helloWorld()
        }

      { _ = self }()
    }

    @MainActor
    init(i4 c:  Bool) async {
      self.x = 0
      self.y = self.x

      Task { self } // expected-note {{after making a copy of 'self', only non-isolated properties of 'self' can be accessed from this init}}

      callMethod(self)

      passInout(&self.x) // expected-warning {{cannot access property 'x' here in non-isolated initializer; this is an error in Swift 6}}
    }
}


@available(SwiftStdlib 5.1, *)
actor X {
    var counter: Int

    init(v1 start: Int) {
        self.counter = start
        Task { await self.setCounter(start + 1) } // expected-note {{after making a copy of 'self', only non-isolated properties of 'self' can be accessed from this init}}

        if self.counter != start { // expected-warning {{cannot access property 'counter' here in non-isolated initializer; this is an error in Swift 6}}
            fatalError("where's my protection?")
        }
    }

    func setCounter(_ x : Int) {
        self.counter = x
    }
}

struct CardboardBox<T> {
    public let item: T
}


@available(SwiftStdlib 5.1, *)
var globalVar: EscapeArtist? // expected-note 2 {{var declared here}}

@available(SwiftStdlib 5.1, *)
actor EscapeArtist {
    var x: Int

    init(attempt1: Bool) {
        self.x = 0

        // expected-note@+2 {{after making a copy of 'self', only non-isolated properties of 'self' can be accessed from this init}}
        // expected-warning@+1 {{reference to var 'globalVar' is not concurrency-safe because it involves shared mutable state}}
        globalVar = self

        // expected-warning@+1 {{reference to var 'globalVar' is not concurrency-safe because it involves shared mutable state}}
        Task { await globalVar!.isolatedMethod() }

        if self.x == 0 {  // expected-warning {{cannot access property 'x' here in non-isolated initializer; this is an error in Swift 6}}
            fatalError("race detected.")
        }
    }

    init(attempt2: Bool) {
        self.x = 0

        let wrapped: EscapeArtist? = .some(self)    // expected-note {{after making a copy of 'self', only non-isolated properties of 'self' can be accessed from this init}}
        let selfUnchained = wrapped!

        Task { await selfUnchained.isolatedMethod() }
        if self.x == 0 {  // expected-warning {{cannot access property 'x' here in non-isolated initializer; this is an error in Swift 6}}
            fatalError("race detected.")
        }
    }

    init(attempt3: Bool) {
        self.x = 0

        let unchainedSelf = self

        unchainedSelf.nonisolated()
    }

    init(attempt4: Bool) {
        self.x = 0

        let unchainedSelf = self

        unchainedSelf.nonisolated()

        let _ = { unchainedSelf.nonisolated() }()
    }

    init(attempt5: Bool) {
        self.x = 0

        let box = CardboardBox(item: self)
        box.item.nonisolated()
    }

    init(attempt6: Bool) {
        self.x = 0
        func fn() {
            self.nonisolated()
        }
        fn()
    }

    func isolatedMethod() { x += 1 }
    nonisolated func nonisolated() {}
}

@available(SwiftStdlib 5.5, *)
actor Ahmad {
  nonisolated func f() {}
  var prop: Int = 0
  var computedProp: Int { 10 } // expected-note {{property declared here}}

  init(v1: Void) {
    Task.detached { self.f() } // expected-note {{after making a copy of 'self', only non-isolated properties of 'self' can be accessed from this init}}
    f()
    prop += 1 // expected-warning {{cannot access property 'prop' here in non-isolated initializer; this is an error in Swift 6}}
  }

  nonisolated init(v2: Void) async {
    Task.detached { self.f() } // expected-note {{after making a copy of 'self', only non-isolated properties of 'self' can be accessed from this init}}
    f()
    prop += 1 // expected-warning {{cannot access property 'prop' here in non-isolated initializer; this is an error in Swift 6}}
  }

  nonisolated init(v3: Void) async {
    prop = 10
    f()       // expected-note {{after calling instance method 'f()', only non-isolated properties of 'self' can be accessed from this init}}
    prop += 1 // expected-warning {{cannot access property 'prop' here in non-isolated initializer; this is an error in Swift 6}}
  }

  @MainActor init(v4: Void) async {
    prop = 10
    f()       // expected-note {{after calling instance method 'f()', only non-isolated properties of 'self' can be accessed from this init}}
    prop += 1 // expected-warning {{cannot access property 'prop' here in non-isolated initializer; this is an error in Swift 6}}
  }

  nonisolated deinit {
    // expected-warning@+2 {{actor-isolated property 'computedProp' can not be referenced from a non-isolated context; this is an error in Swift 6}}
    // expected-note@+1 {{after accessing property 'computedProp', only non-isolated properties of 'self' can be accessed from a deinit}}
    let x = computedProp

    prop = x // expected-warning {{cannot access property 'prop' here in deinitializer; this is an error in Swift 6}}
  }
}

@available(SwiftStdlib 5.5, *)
actor Rain {
  var x: Int = 0
  nonisolated func f() {}

  init(_ hollerBack: (Rain) -> () -> Void) {
    defer { self.f() }

    defer { _ = self.x }  // expected-warning {{cannot access property 'x' here in non-isolated initializer; this is an error in Swift 6}}

    defer { Task { self.f() } }

    defer { _ = hollerBack(self) } // expected-note {{after a call involving 'self', only non-isolated properties of 'self' can be accessed from this init}}
  }

  init(_ hollerBack: (Rain) -> () -> Void) async {
    defer { self.f() }

    defer { _ = self.x }

    defer { Task { self.f() } }

    defer { _ = hollerBack(self) }
  }

  deinit {
    x = 1
  }
}

@available(SwiftStdlib 5.5, *)
actor NonIsolatedDeinitExceptionForSwift5 {
  var x: Int = 0

  func cleanup() { // expected-note {{calls to instance method 'cleanup()' from outside of its actor context are implicitly asynchronous}}
    x = 0
  }

  deinit {
    // expected-warning@+2 {{actor-isolated instance method 'cleanup()' can not be referenced from a non-isolated context; this is an error in Swift 6}}
    // expected-note@+1 {{after calling instance method 'cleanup()', only non-isolated properties of 'self' can be accessed from a deinit}}
    cleanup()

    x = 1 // expected-warning {{cannot access property 'x' here in deinitializer; this is an error in Swift 6}}
  }
}

@available(SwiftStdlib 5.5, *)
actor IsolatedDeinitExceptionForSwift5 {
  var x: Int = 0

  func cleanup() {
    x = 0
  }

  isolated deinit {
    cleanup() // ok

    x = 1 // ok
  }
}


@available(SwiftStdlib 5.5, *)
actor OhBrother {
  private var giver: (OhBrother) -> Int
  private var whatever: Int = 0

  static var DefaultResult: Int { 10 }

  init() {
    // expected-note@+2 {{after this closure involving 'self', only non-isolated properties of 'self' can be accessed from this init}}
    // expected-warning@+1 {{cannot access property 'giver' here in non-isolated initializer; this is an error in Swift 6}}
    self.giver = { (x: OhBrother) -> Int in Self.DefaultResult }
  }

  init(v2: Void) {
    giver = { (x: OhBrother) -> Int in 0 }

    // make sure we don't call this a closure, which is the more common situation.

    _ = giver(self) // expected-note {{after a call involving 'self', only non-isolated properties of 'self' can be accessed from this init}}

    whatever = 1 // expected-warning {{cannot access property 'whatever' here in non-isolated initializer; this is an error in Swift 6}}
  }

  init(v3: Void) {
    let blah = { (x: OhBrother) -> Int in 0 }
    giver = blah

    // TODO: would be nice if we didn't say "after this closure" since it's not a capture, but a call.

    _ = blah(self) // expected-note {{after this closure involving 'self', only non-isolated properties of 'self' can be accessed from this init}}

    whatever = 2 // expected-warning {{cannot access property 'whatever' here in non-isolated initializer; this is an error in Swift 6}}
  }
}

@available(SwiftStdlib 5.1, *)
@MainActor class AwesomeUIView {}

@available(SwiftStdlib 5.1, *)
class CheckDeinitFromClass: AwesomeUIView {
  var ns: NonSendableType?
  deinit {
    ns?.f() // expected-warning {{cannot access property 'ns' with a non-sendable type 'NonSendableType?' from non-isolated deinit; this is an error in Swift 6}}
    ns = nil // expected-warning {{cannot access property 'ns' with a non-sendable type 'NonSendableType?' from non-isolated deinit; this is an error in Swift 6}}
  }
}

@available(SwiftStdlib 5.1, *)
actor CheckDeinitFromActor {
  var ns: NonSendableType?
  deinit {
    ns?.f() // expected-warning {{cannot access property 'ns' with a non-sendable type 'NonSendableType?' from non-isolated deinit; this is an error in Swift 6}}
    ns = nil // expected-warning {{cannot access property 'ns' with a non-sendable type 'NonSendableType?' from non-isolated deinit; this is an error in Swift 6}}
  }
}
