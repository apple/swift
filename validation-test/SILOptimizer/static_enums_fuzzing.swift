// RUN: %empty-directory(%t) 
// RUN: %swift_driver %s >%t/static_enums.swift

// RUN: %target-build-swift -parse-as-library -O %t/static_enums.swift -module-name=test -o %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %t/static_enums.swift

// REQUIRES: executable_test,swift_stdlib_no_asserts,optimized_stdlib
// REQUIRES: swift_in_compiler

// Note: This code is not the testfile itself but generates the testfile in the %t directory.

createTestfile()

func createTestfile() {
  let globals = createGlobals(count: 500)

  print(typeDefinitions)

  for (idx, t) in globals.enumerated() {
    print("public var global\(idx)var: \(t.getType()) = \(t.getInitValue())")
  }

  print()

  for (idx, _) in globals.enumerated() {
    print("""
          @inline(never) func printGlobal\(idx)() {
            print("global\(idx)var: \", global\(idx)var as Any)
          }
          """)
  }

  print(mainFunctionProlog)

  for (idx, t) in globals.enumerated() {
    print("""
              // CHECK: global\(idx)var: \(t.getExpectedOutput(topLevel: true))
              printGlobal\(idx)()
          """)
  }

  print(mainFunctionEpilog)
}

func createGlobals(count: Int) -> [any Value] {
  var globals = [any Value]()
  var avoidDuplicates = Set<String>()

  var generator = RandomGenerator()

  var numCreated = 0
  while numCreated < count {
    let t = generator.createValue(depth: 3)
    if t.containsEnum {
      let key = "\(t.getType())=\(t.getInitValue())"
      if avoidDuplicates.insert(key).inserted {
        globals.append(t)
        numCreated += 1
      }
    }
  }
  return globals
}

var typeDefinitions: String {
  """
    public struct S<T, V> {
      var a: T
      var b: V
    }

    public enum E {
      case X, Y, Z
    }

    public enum MPE<T, V> {
      case A(T)
      case B(V)
      case C
    }

    public enum E24 {
      case A
      case B(UInt8, UInt8, UInt8)
    }

    public func fn() {}

    public typealias Func = () -> ()

  """
}

var mainFunctionProlog: String {
  """

  @main
  struct Main {
    static func main() {
  """
}

var mainFunctionEpilog: String {
  """
    }
  }
  """
}

protocol Value {
  init(generator: inout RandomGenerator, depth: Int)

  func getType() -> String
  func getInitValue() -> String
  func getExpectedOutput(topLevel: Bool) -> String
  func getRuntimeTypeName(topLevel: Bool) -> String
  var containsEnum: Bool { get }
}

extension Value {
  func getExpectedOutput(topLevel: Bool) -> String {
    return getInitValue()
  }
  var containsEnum: Bool { false }
}

struct SmallInt : Value {

  init(generator: inout RandomGenerator, depth: Int) {}

  func getType() -> String { "Int8" }
  func getInitValue() -> String  { "42" }
  func getRuntimeTypeName(topLevel: Bool) -> String { topLevel ? "Int8" : "Swift.Int8" }
}

struct LargeInt : Value {

  init(generator: inout RandomGenerator, depth: Int) {}

  func getType() -> String { "Int64" }
  func getInitValue() -> String  { "1311768468603649711" }
  func getRuntimeTypeName(topLevel: Bool) -> String { topLevel ? "Int64" : "Swift.Int64" }
}

struct SmallString : Value {

  init(generator: inout RandomGenerator, depth: Int) {}

  func getType() -> String { "String" }
  func getInitValue() -> String  { "\"small\"" }
  func getRuntimeTypeName(topLevel: Bool) -> String { topLevel ? "String" : "Swift.String" }
}

struct LargeString : Value {

  init(generator: inout RandomGenerator, depth: Int) {}

  func getType() -> String { "String" }
  func getInitValue() -> String  { "\"large string which exceeds the inline buffer\"" }
  func getRuntimeTypeName(topLevel: Bool) -> String { topLevel ? "String" : "Swift.String" }
}

struct Function : Value {

  init(generator: inout RandomGenerator, depth: Int) {}

  func getType() -> String { "Func" }
  func getInitValue() -> String  { "fn" }
  func getRuntimeTypeName(topLevel: Bool) -> String { "() -> ()" }
  func getExpectedOutput(topLevel: Bool) -> String { "(Function)" }
}

struct OptionalValue : Value {

  let payload: any Value
  let isSome: Bool

  init(generator: inout RandomGenerator, depth: Int) {
    self.isSome = Bool.random(using: &generator)
    self.payload = generator.createValue(depth: depth)
  }
  

  func getType() -> String {
    payload.getType() + "?"
  }

  func getInitValue() -> String  {
    if isSome {
      if let plOpt = payload as? OptionalValue {
        if !plOpt.isSome {
          return "Optional(Optional(nil))"
        }
        return "Optional(\(payload.getInitValue()))"
      } else {
        return "\(payload.getInitValue())"
      }
    } else {
      return "nil"
    }
  }

  func getExpectedOutput(topLevel: Bool) -> String  {
    if isSome {
      return "Optional(\(payload.getExpectedOutput(topLevel: false)))"
    } else {
      return "nil"
    }
  }

  func getRuntimeTypeName(topLevel: Bool) -> String {
    let prefix = topLevel ? "" : "Swift."
    return "\(prefix)Optional<\(payload.getRuntimeTypeName(topLevel: topLevel))>"
  }

  var containsEnum: Bool { true }
}

struct Struct : Value {

  let a: any Value
  let b: any Value

  init(generator: inout RandomGenerator, depth: Int) {
    self.a = generator.createValue(depth: depth)
    self.b = generator.createValue(depth: depth)
  }
  

  func getType() -> String {
    "S<\(a.getType()), \(b.getType())>"
  }

  func getInitValue() -> String  {
    "S(a: \(a.getInitValue()), b: \(b.getInitValue()))"
  }

  func getExpectedOutput(topLevel: Bool) -> String  {
    "\(getRuntimeTypeName(topLevel: topLevel))(a: \(a.getExpectedOutput(topLevel: false)), b: \(b.getExpectedOutput(topLevel: false)))"
  }

  func getRuntimeTypeName(topLevel: Bool) -> String {
    let prefix = topLevel ? "" : "test."
    return "\(prefix)S<\(a.getRuntimeTypeName(topLevel: topLevel)), \(b.getRuntimeTypeName(topLevel: topLevel))>"
  }

  var containsEnum: Bool { a.containsEnum || b.containsEnum }
}

struct Enum : Value {
  let caseIdx: Int
  
  init(generator: inout RandomGenerator, depth: Int) {
    self.caseIdx = Int.random(in: 0..<3, using: &generator)
  }
  

  func getType() -> String {
    "E"
  }

  func getInitValue() -> String  {
    "E.\(caseName)"
  }

  func getExpectedOutput(topLevel: Bool) -> String  {
    let prefix = topLevel ? "" : "test.E."
    return "\(prefix)\(caseName)"
  }

  func getRuntimeTypeName(topLevel: Bool) -> String {
    let prefix = topLevel ? "" : "test."
    return "\(prefix)E"
  }

  var containsEnum: Bool { true }

  private var caseName: String {
    switch caseIdx {
      case 0: return "X"
      case 1: return "Y"
      case 2: return "Z"
      default: fatalError()
    }
  }
}

struct MultiPayloadEnum : Value {
  let payloadA: any Value
  let payloadB: any Value
  let caseIdx: Int
  
  init(generator: inout RandomGenerator, depth: Int) {
    self.caseIdx = Int.random(in: 0..<3, using: &generator)
    self.payloadA = generator.createValue(depth: depth)
    self.payloadB = generator.createValue(depth: depth)
  }
  
  func getType() -> String {
    "MPE<\(payloadA.getType()), \(payloadB.getType())>"
  }

  func getInitValue() -> String  {
    switch caseIdx {
      case 0: return "MPE.A(\(payloadA.getInitValue()))"
      case 1: return "MPE.B(\(payloadB.getInitValue()))"
      case 2: return "MPE.C"
      default: fatalError()
    }
  }

  func getExpectedOutput(topLevel: Bool) -> String  {
    let prefix = topLevel ? "" : "\(getRuntimeTypeName(topLevel: topLevel))."
    switch caseIdx {
      case 0: return "\(prefix)A(\(payloadA.getExpectedOutput(topLevel: false)))"
      case 1: return "\(prefix)B(\(payloadB.getExpectedOutput(topLevel: false)))"
      case 2: return "\(prefix)C"
      default: fatalError()
    }
  }

  func getRuntimeTypeName(topLevel: Bool) -> String {
    let prefix = topLevel ? "" : "test."
    return "\(prefix)MPE<\(payloadA.getRuntimeTypeName(topLevel: topLevel)), \(payloadB.getRuntimeTypeName(topLevel: topLevel))>"
  }

  var containsEnum: Bool { true }
}

struct Size24Enum : Value {
  let caseIdx: Int
  
  init(generator: inout RandomGenerator, depth: Int) {
    self.caseIdx = Int.random(in: 0..<2, using: &generator)
  }
  
  func getType() -> String {
    "E24"
  }

  func getInitValue() -> String  {
    switch caseIdx {
      case 0: return "E24.A"
      case 1: return "E24.B(250, 128, 114)"
      default: fatalError()
    }
  }

  func getExpectedOutput(topLevel: Bool) -> String  {
    let prefix = topLevel ? "" : "\(getRuntimeTypeName(topLevel: topLevel))."
    switch caseIdx {
      case 0: return "\(prefix)A"
      case 1: return "\(prefix)B(250, 128, 114)"
      default: fatalError()
    }
  }

  func getRuntimeTypeName(topLevel: Bool) -> String {
    let prefix = topLevel ? "" : "test."
    return "\(prefix)E24"
  }

  var containsEnum: Bool { true }
}

// Can't use the default random generator becaus we need deterministic results
struct RandomGenerator : RandomNumberGenerator {
  var state: (UInt64, UInt64, UInt64, UInt64) = (15042304078070129153, 10706435816813474385, 14710304063852993123, 11070704559760783939)

  mutating func next() -> UInt64 {
    let result = rotl(state.1 &* 5, 7) &* 9

    let t = state.1 << 17
    state.2 ^= state.0
    state.3 ^= state.1
    state.1 ^= state.2
    state.0 ^= state.3
    state.2 ^= t
    state.3 = rotl(state.3, 45)

    return result
  }

  private func rotl(_ x: UInt64, _ k: UInt64) -> UInt64 {
    return (x << k) | (x >> (64 &- k))
  }

  mutating func createValue(depth: Int) -> any Value {
    if depth > 0 {
      let idx = Int.random(in: 0..<Self.allValueTypes.count, using: &self)
      return Self.allValueTypes[idx].init(generator: &self, depth: depth - 1)
    } else {
      let idx = Int.random(in: 0..<Self.allTerminalTypes.count, using: &self)
      return Self.allTerminalTypes[idx].init(generator: &self, depth: 0)
    }
  }

  private static let allTerminalTypes: [any Value.Type] = [
    SmallInt.self,
    LargeInt.self,
    SmallString.self,
    LargeString.self,
    Function.self,
    Enum.self,
    Size24Enum.self
  ]
  private static let allValueTypes: [any Value.Type] = allTerminalTypes + [
    OptionalValue.self,
    Struct.self,
    MultiPayloadEnum.self
  ]

}

