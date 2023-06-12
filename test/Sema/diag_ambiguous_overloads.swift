// RUN: %target-typecheck-verify-swift

enum E : String {
  case foo = "foo"
  case bar = "bar" // expected-note {{'bar' declared here}}
}
func fe(_: E) {}
func fe(_: Int) {}
func fe(_: Int, _: E) {}
func fe(_: Int, _: Int) {}

fe(E.baz) // expected-error {{type 'E' has no member 'baz'; did you mean 'bar'?}}
fe(.baz) // expected-error {{reference to member 'baz' cannot be resolved without a contextual type}}

fe(.nope, .nyet) // expected-error {{type 'Int' has no member 'nope'}}
// expected-error@-1 {{reference to member 'nyet' cannot be resolved without a contextual type}}

func fg<T>(_ f: (T) -> T) -> Void {}
fg({x in x}) // expected-error {{cannot infer type of closure parameter 'x' without a type annotation}}


struct S {
  func f<T>(_ i: (T) -> T, _ j: Int) -> Void {}
  func f(_ d: (Double) -> Double) -> Void {}
  func test() -> Void {
    f({x in x}, 2) // expected-error {{cannot infer type of closure parameter 'x' without a type annotation}}
  }
  
  func g<T>(_ a: T, _ b: Int) -> Void {}
  func g(_ a: String) -> Void {}
  func test2() -> Void {
    g(.notAThing, 7) // expected-error {{cannot infer contextual base in reference to member 'notAThing'}}
  }
  
  func h(_ a: Int, _ b: Int) -> Void {}
  func h(_ a: String) -> Void {}
  func test3() -> Void {
    h(.notAThing, 3) // expected-error {{type 'Int' has no member 'notAThing'}}
    h(.notAThing) // expected-error {{type 'String' has no member 'notAThing'}}
  }
}

struct School {
  var name: String
}
func testDiagnoseForAmbiguityCrash(schools: [School]) {
  schools.map({ $0.name }).sorted(by: { $0.nothing < $1.notAThing })
  // expected-error@-1 {{value of type 'String' has no member 'nothing'}}
  // expected-error@-2 {{value of type 'String' has no member 'notAThing'}}
}

class DefaultValue {
  static func foo(_ a: Int) {}
  static func foo(_ a: Int, _ b: Int = 1) {}
  static func foo(_ a: Int, _ b: Int = 1, _ c: Int = 2) {}
}
DefaultValue.foo(1.0, 1) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}


class Variadic {
  static func foo(_ a: Int) {}
  static func foo(_ a: Int, _ b: Double...) {}
}
Variadic.foo(1.0, 2.0, 3.0) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}


// https://github.com/apple/swift/issues/50453
do {
  class Suit {
    static func foo<T: Any>(_ :inout T) {}
    static func foo() {}
  }

  class RandomNumberGenerator {}

  let myRNG = RandomNumberGenerator() // expected-note {{change 'let' to 'var' to make it mutable}}
  _ = Suit.foo(&myRNG) // expected-error {{cannot pass immutable value as inout argument: 'myRNG' is a 'let' constant}}
}

// https://github.com/apple/swift/issues/50325
do {
  struct S {
    func foo() -> UInt { return 0 }
    func foo<T: UnsignedInteger>(bar: T) -> T { // expected-note {{where 'T' = 'Int'}}
      return bar
    }
  }

  let s = S()
  let a = s.foo()
  let b = s.foo(bar: 123) // expected-error {{instance method 'foo(bar:)' requires that 'Int' conform to 'UnsignedInteger'}}
  let c: UInt = s.foo(bar: 123)
  let d = s.foo(bar: 123 as UInt)
}

// https://github.com/apple/swift/issues/49983
do {
  struct ITunesGenre {
    let genre: Int // expected-note {{'genre' declared here}}
    let name: String
  }
  class Genre {
    static func fetch<B: BinaryInteger>(genreID: B, name: String) {}
    static func fetch(_ iTunesGenre: ITunesGenre) -> Genre {
      return Genre.fetch(genreID: iTunesGenre.genreID, name: iTunesGenre.name)
      // expected-error@-1 {{value of type 'ITunesGenre' has no member 'genreID'; did you mean 'genre'?}}
      // expected-error@-2 {{cannot convert return expression of type '()' to return type 'Genre'}}
    }
  }
}

// https://github.com/apple/swift/issues/47730

protocol Scheduler {
  func inBackground(run task: @escaping () -> Void)
}

extension Scheduler {
  func inBackground(run task: @escaping () -> [Count],
                    completedBy resultHandler: @escaping ([Count]) -> Void) {}
}

struct Count { // expected-note {{'init(title:)' declared here}}
  let title: String
}

func getCounts(_ scheduler: Scheduler, _ handler: @escaping ([Count]) -> Void) {
  scheduler.inBackground(run: {
    return [Count()] // expected-error {{missing argument for parameter 'title' in call}}
  }, completedBy: { // expected-error {{contextual type for closure argument list expects 1 argument, which cannot be implicitly ignored}} {{20-20= _ in}}
  })
}

// https://github.com/apple/swift/issues/55133
do {
  func f(_ u: UnsafeBufferPointer<UInt16>) {}

  let array : [UInt16]

  array.withUnsafeBufferPointer {
    _ = f(UnsafeRawPointer($0).bindMemory(to: UInt16.self, capacity: 1)) // expected-error {{cannot convert value of type 'UnsafePointer<UInt16>' to expected argument type 'UnsafeBufferPointer<UInt16>'}}
    // expected-error@-1 {{cannot convert value of type 'UnsafeBufferPointer<UInt16>' to expected argument type 'UnsafeMutableRawPointer'}}
  }

  array.withUnsafeBufferPointer {
    _ = UnsafeRawPointer($0) as UnsafeBufferPointer<UInt16> // expected-error {{cannot convert value of type 'UnsafeRawPointer' to type 'UnsafeBufferPointer<UInt16>' in coercion}}
    // expected-error@-1 {{cannot convert value of type 'UnsafeBufferPointer<UInt16>' to expected argument type 'UnsafeMutableRawPointer'}}
  }
}
do {
  func f1(_ u: Int) -> String {} // expected-note 2 {{candidate expects value of type 'Int' for parameter #1 (got 'Double')}}
  func f1(_ u: String) -> Double {} // expected-note 2 {{candidate expects value of type 'String' for parameter #1 (got 'Double')}}
  func f2(_ u: Int) {}

  f2(f1(1 as Double)) // expected-error {{no exact matches in call to local function 'f1'}}
  f1(1 as Double) as Int // expected-error {{no exact matches in call to local function 'f1'}}
}
do {
  // Ambiguous OverloadRefExpr
  func f1(_ p: Int) {} // expected-note {{found candidate with type '(Int) -> ()'}}
  func f1(_ p: Double) {} // expected-note {{found candidate with type '(Double) -> ()'}}
  func f2(_ param: (String)-> Void) {}

  f2(f1) // expected-error {{no 'f1' candidates produce the expected type '(String) -> Void' for parameter #0}}
}
