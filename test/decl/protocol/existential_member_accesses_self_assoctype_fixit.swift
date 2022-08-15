// RUN: %target-typecheck-verify-swift

protocol P {
  associatedtype A

  func method(_: A)
  subscript(_: A) -> A { get }
  init(_: A)

  static func staticMethod(_: A)
}
protocol Q {}

do {
  func test(p: P) { // expected-error {{use of protocol 'P' as a type must be written 'any P'}}
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{-1:16--1:17=some P}} {{none}}
  }
}
do {
  func test(p: ((P))) { // expected-error {{use of protocol 'P' as a type must be written 'any P'}}
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{-1:18--1:19=some P}} {{none}}
  }
}
do {
  func test(p: any P) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{-1:20--1:21=some P}} {{-1:16--1:20=}} {{none}}
  }
}
do {
  func test(p: (inout any P)) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{none}}
  }
}
do {
  func test(p: __shared (any P)) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{-1:30--1:31=some P}} {{-1:26--1:30=}} {{none}}
  }
}
do {
  func test(p: ((any P))) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{-1:22--1:23=some P}} {{-1:18--1:22=}} {{none}}
  }
}
do {
  func test(p: any (P)) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{-1:21--1:22=some P}} {{-1:16--1:20=}} {{none}}
  }
}
do {
  func test(p: any   P) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{-1:22--1:23=some P}} {{-1:16--1:22=}} {{none}}
  }
}
do {
  func test(p: (any  (P))) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{-1:23--1:24=some P}} {{-1:17--1:22=}} {{none}}
  }
}
do {
  func test(p: P.Type) { // expected-error {{use of protocol 'P' as a type must be written 'any P'}}
    p.staticMethod(false) // expected-error {{member 'staticMethod' cannot be used on value of type 'any P.Type'; consider using a generic constraint instead}} {{-1:16--1:17=(some P)}} {{none}}
  }
}
do {
  func test(p: (P).Type) { // expected-error {{use of protocol 'P' as a type must be written 'any P'}}
    p.staticMethod(false) // expected-error {{member 'staticMethod' cannot be used on value of type 'any P.Type'; consider using a generic constraint instead}} {{-1:17--1:18=some P}} {{none}}
  }
}
do {
  func test(p: any P.Type) {
    p.staticMethod(false) // expected-error {{member 'staticMethod' cannot be used on value of type 'any P.Type'; consider using a generic constraint instead}} {{-1:20--1:21=(some P)}} {{-1:16--1:20=}} {{none}}
  }
}
do {
  func test(p: any ((P).Type)) {
    p.staticMethod(false) // expected-error {{member 'staticMethod' cannot be used on value of type 'any P.Type'; consider using a generic constraint instead}} {{-1:22--1:23=some P}} {{-1:16--1:20=}} {{none}}
  }
}

do {
  func test(p: P & Q) { // expected-error {{use of protocol 'P' as a type must be written 'any P'}}
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P & Q'; consider using a generic constraint instead}} {{-1:16--1:21=some P & Q}} {{none}}
  }
}
do {
  func test(p: ((P & Q))) { // expected-error {{use of protocol 'P' as a type must be written 'any P'}}
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P & Q'; consider using a generic constraint instead}} {{-1:18--1:23=some P & Q}} {{none}}
  }
}
do {
  func test(p: any P & Q) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P & Q'; consider using a generic constraint instead}} {{-1:20--1:25=some P & Q}} {{-1:16--1:20=}} {{none}}
  }
}
do {
  func test(p: inout any P & Q) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P & Q'; consider using a generic constraint instead}} {{none}}
  }
}
do {
  func test(p: __shared (any P & Q)) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P & Q'; consider using a generic constraint instead}} {{-1:30--1:35=some P & Q}} {{-1:26--1:30=}} {{none}}
  }
}
do {
  func test(p: ((any P & Q))) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P & Q'; consider using a generic constraint instead}} {{-1:22--1:27=some P & Q}} {{-1:18--1:22=}} {{none}}
  }
}
do {
  func test(p: any (P & Q)) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P & Q'; consider using a generic constraint instead}} {{-1:21--1:26=some P & Q}} {{-1:16--1:20=}} {{none}}
  }
}
do {
  func test(p: any   P & Q) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P & Q'; consider using a generic constraint instead}} {{-1:22--1:27=some P & Q}} {{-1:16--1:22=}} {{none}}
  }
}
do {
  func test(p: (any  (P & Q))) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P & Q'; consider using a generic constraint instead}} {{-1:23--1:28=some P & Q}} {{-1:17--1:22=}} {{none}}
  }
}
do {
  func test(p: (P & Q).Type) { // expected-error {{use of protocol 'P' as a type must be written 'any P'}}
    p.staticMethod(false) // expected-error {{member 'staticMethod' cannot be used on value of type 'any (P & Q).Type'; consider using a generic constraint instead}} {{-1:17--1:22=some P & Q}} {{none}}
  }
}
do {
  func test(p: ((P & Q)).Type) { // expected-error {{use of protocol 'P' as a type must be written 'any P'}}
    p.staticMethod(false) // expected-error {{member 'staticMethod' cannot be used on value of type 'any (P & Q).Type'; consider using a generic constraint instead}} {{-1:18--1:23=some P & Q}} {{none}}
  }
}
do {
  func test(p: any (P & Q).Type) {
    p.staticMethod(false) // expected-error {{member 'staticMethod' cannot be used on value of type 'any (P & Q).Type'; consider using a generic constraint instead}} {{-1:21--1:26=some P & Q}} {{-1:16--1:20=}} {{none}}
  }
}
do {
  func test(p: any (((P & Q)).Type)) {
    p.staticMethod(false) // expected-error {{member 'staticMethod' cannot be used on value of type 'any (P & Q).Type'; consider using a generic constraint instead}} {{-1:23--1:28=some P & Q}} {{-1:16--1:20=}} {{none}}
  }
}

do {
  // With an existing generic parameter list.
  func test<T: Sequence>(t: T, p: any P) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{-1:39--1:40=some P}} {{-1:35--1:39=}} {{none}}
  }
}
do {
  // With an subscript expression.
  func test(p: any P) {
    p[false] // expected-error {{member 'subscript' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{-1:20--1:21=some P}} {{-1:16--1:20=}} {{none}}
  }
}
do {
  // With an initializer.
  func test(p: any P.Type) {
    p.init(false) // expected-error {{member 'init' cannot be used on value of type 'any P.Type'; consider using a generic constraint instead}} {{-1:20--1:21=(some P)}} {{-1:16--1:20=}} {{none}}
  }
}

// Inside initializers, accessors and subscripts.
struct Test {
  init(p: any P) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{-1:15--1:16=some P}} {{-1:11--1:15=}} {{none}}
  }

  init<T: P>(p: any P, t: T) {
    p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{-1:21--1:22=some P}} {{-1:17--1:21=}} {{none}}
  }

  subscript(p: any P) -> any P {
    p.method(false)  // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{-1:20--1:21=some P}} {{-1:16--1:20=}} {{none}}
    return p
  }

  subscript<T: P>(p: any P, t: T) -> any P {
    get {
      p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{-2:26--2:27=some P}} {{-2:22--2:26=}} {{none}}
      return p
    }
    set(value) {
      p.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{-6:26--6:27=some P}} {{-6:22--6:26=}} {{none}}
      value.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{none}}
    }
  }

  subscript(p: any P & Q) -> any P {
    _read { p.method(false) }   // expected-error {{member 'method' cannot be used on value of type 'any P & Q'; consider using a generic constraint instead}} {{-1:20--1:25=some P & Q}} {{-1:16--1:20=}} {{none}}
    _modify { p.method(false) } // expected-error {{member 'method' cannot be used on value of type 'any P & Q'; consider using a generic constraint instead}} {{-2:20--2:25=some P & Q}} {{-2:16--2:20=}} {{none}}
    willSet { p.method(false) } // expected-error {{member 'method' cannot be used on value of type 'any P & Q'; consider using a generic constraint instead}} {{-3:20--3:25=some P & Q}} {{-3:16--3:20=}} {{none}}
    didSet { p.method(false) }  // expected-error {{member 'method' cannot be used on value of type 'any P & Q'; consider using a generic constraint instead}} {{-4:20--4:25=some P & Q}} {{-4:16--4:20=}} {{none}}
    // expected-error@-2 {{'willSet' is not allowed in subscripts}}
    // expected-error@-2 {{'didSet' is not allowed in subscripts}}
  }

  var property: any P {
    get {}
    set {
      newValue.method(false) // expected-error {{member 'method' cannot be used on value of type 'any P'; consider using a generic constraint instead}} {{none}}
    }
  }
}
