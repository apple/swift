class Person {
  var firstName: String!
  var lastName: String!
  var age: Int!
  var planet = "Earth", solarSystem = "Milky Way"
  var avgHeight = 175
  let line = #line, file = #file, handle = #dsohandle
  lazy var idea: Idea = { fatalError() }()
  var location: () throws -> Place<Int> = { fatalError() }
  var secondLocation: (() -> Place<Int>)!
  @MyWrapper var wrapped: String = ""
  var computed: String { "hi" }
  var getSet: String {
    get { "hi" }
    set {}
  }
  var didSet: String = "ds" {
    didSet { print("didSet") }
  }
}

struct Place<T: FixedWidthInteger> {
  typealias Callback = () -> ()
  let number: T
  let person: Person
  let street: String
  let apartment: Optional<String>
  let city: String
  let state: String
  let postalCode: Int
  let plusFour: Int?
  let callback: Callback
  @MyWrapper var wrapped: String
  var `protocol`: String
}

protocol Thing {
  var idea: Idea { get }
}

enum Idea {
  var subject: String { fatalError() }
}

struct OnlyComputed {
  lazy var idea: Idea = { fatalError() }()
  var computed: String { "hi" }
}

@propertyWrapper
struct MyWrapper {
  let wrappedValue: String
}

// RUN: %empty-directory(%t.result)
// RUN: %refactor -memberwise-init -source-filename %s -pos=1:8 > %t.result/generate_memberwise.swift
// RUN: diff -u %S/Outputs/generate_memberwise/class_members.swift.expected %t.result/generate_memberwise.swift

// RUN: %refactor -memberwise-init -source-filename %s -pos=22:8 > %t.result/struct_members.swift
// RUN: diff -u %S/Outputs/generate_memberwise/struct_members.swift.expected %t.result/struct_members.swift

// RUN: %refactor -memberwise-init -source-filename %s -pos=45:8 > %t.result/only_computed_members.swift
// RUN: diff -u %S/Outputs/generate_memberwise/only_computed_members.swift.expected %t.result/only_computed_members.swift

// RUN: not %refactor -memberwise-init -source-filename %s -pos=37:10 > %t.result/protocol_members.swift
// RUN: not %refactor -memberwise-init -source-filename %s -pos=41:6 > %t.result/enum_members.swift

