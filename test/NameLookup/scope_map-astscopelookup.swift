
// Note: test of the scope map. All of these tests are line- and
// column-sensitive, so any additions should go at the end.

struct S0 {
  class InnerC0 { }
}

extension S0 {
}

class C0 {
}

enum E0 {
  case C0
  case C1(Int, Int)
}

struct GenericS0<T, U> {
}

func genericFunc0<T, U>(t: T, u: U, i: Int = 10) {
}

class ContainsGenerics0 {
  init<T, U>(t: T, u: U) {
  }

  deinit {
  }
}

typealias GenericAlias0<T> = [T]

#if arch(unknown)
struct UnknownArchStruct { }
#else
struct OtherArchStruct { }
#endif

func functionBodies1(a: Int, b: Int?) {
  let (x1, x2) = (a, b),
      (y1, y2) = (b, a)
  let (z1, z2) = (a, a)
  do {
    let a1 = a
    let a2 = a
    do {
      let b1 = b
      let b2 = b
    }
  }
  do {
    let b1 = b
    let b2 = b
  }
  func f(_ i: Int) -> Int { return i }
  let f2 = f(_:)
  struct S7 { }
  typealias S7Alias = S7
  
  if let b1 = b, let b2 = b {
    let c1 = b
  } else {
    let c2 = b
  }

  guard let b1 = b, { $0 > 5 }(b1), let b2 = b else {
    let c = 5
    return
  }

  while let b3 = b, let b4 = b {
    let c = 5
  }

  repeat { } while true;

  for (x, y) in [(1, "hello"), (2, "world")] where x % 2 == 0 {

  }

  do {
    try throwing()
  } catch let mine as MyError where mine.value == 17 {
  } catch {
  }

  switch MyEnum.second(1) {
  case .second(let x) where x == 17:
    break;

  case .first:
    break;

  default:
    break;
  }
  for (var i = 0; i != 10; i += 1) { }
}

func throwing() throws { }

struct MyError : Error {
  var value: Int
}

enum MyEnum {
  case first
  case second(Int)
  case third
}

struct StructContainsAbstractStorageDecls {
  subscript (i: Int, j: Int) -> Int {
    set {
    }
    get {
      return i + j
    }
  }

  var computed: Int {
    get {
      return 0
    }
    set {
    }
  }
}

class ClassWithComputedProperties {
  var willSetProperty: Int = 0 {
    willSet { }
  } 

  var didSetProperty: Int = 0 {
    didSet { }
  } 
}

func funcWithComputedProperties(i: Int) {
  var computed: Int {
    set {
    }
    get {
      return 0
    }
  }, var (stored1, stored2) = (1, 2),
  var alsoComputed: Int {
    return 17
  }
    
  do { }
}

func closures() {
  { x, y in 
    return { $0 + $1 }(x, y)
  }(1, 2) + 
  { a, b in a * b }(3, 4)
}

{ closures() }()

func defaultArguments(i: Int = 1,
                      j: Int = { $0 + $1 }(1, 2)) {

  func localWithDefaults(i: Int = 1,
                         j: Int = { $0 + $1 }(1, 2)) {
  }

  let a = i + j
  { $0 }(a)
}

struct PatternInitializers {
  var (a, b) = (1, 2),
      (c, d) = (1.5, 2.5)
}

protocol ProtoWithSubscript {
  subscript(native: Int) -> Int { get set }
}

func localPatternsWithSharedType() {
  let i, j, k: Int
}

class LazyProperties {
  var value: Int = 17

  lazy var prop: Int = self.value
}

func HasLabeledDo() {
  label:
  do {
    for x in 0..<100 {
      if x % 3 == 0 {
        break label
      }
    }
  }
}

// RUN: not %target-swift-frontend -dump-scope-maps expanded %s 2> %t.expanded
// RUN: %FileCheck -check-prefix CHECK-EXPANDED %s < %t.expanded

// CHECK-EXPANDED: ***Complete scope map***
// CHECK-EXPANDED-NEXT: ASTSourceFileScope {{.*}}, [1:1 - 480:1] '{{.*}}scope_map-astscopelookup.swift'
// CHECK-EXPANDED-NEXT: |-NominalTypeDeclScope {{.*}}, [5:1 - 7:1] 'S0'
// CHECK-EXPANDED-NEXT:   `-NominalTypeBodyScope {{.*}}, [5:11 - 7:1] 'S0'
// CHECK-EXPANDED-NEXT:     `-NominalTypeDeclScope {{.*}}, [6:3 - 6:19] 'InnerC0'
// CHECK-EXPANDED-NEXT:       `-NominalTypeBodyScope {{.*}}, [6:17 - 6:19] 'InnerC0'
// CHECK-EXPANDED-NEXT: |-ExtensionDeclScope {{.*}}, [9:13 - 10:1] 'S0'
// CHECK-EXPANDED-NEXT:   `-ExtensionBodyScope {{.*}}, [9:14 - 10:1] 'S0'
// CHECK-EXPANDED-NEXT: |-NominalTypeDeclScope {{.*}}, [12:1 - 13:1] 'C0'
// CHECK-EXPANDED-NEXT:   `-NominalTypeBodyScope {{.*}}, [12:10 - 13:1] 'C0'
// CHECK-EXPANDED-NEXT: |-NominalTypeDeclScope {{.*}}, [15:1 - 18:1] 'E0'
// CHECK-EXPANDED-NEXT:   `-NominalTypeBodyScope {{.*}}, [15:9 - 18:1] 'E0'
// CHECK-EXPANDED-NEXT:     |-EnumElementScope {{.*}}, [16:8 - 16:8] 
// CHECK-EXPANDED-NEXT:     `-EnumElementScope {{.*}}, [17:8 - 17:19] 
// CHECK-EXPANDED-NEXT:       `-ParameterListScope {{.*}}, [17:10 - 17:19] 
// CHECK-EXPANDED-NEXT: |-NominalTypeDeclScope {{.*}}, [20:1 - 21:1] 'GenericS0'
// CHECK-EXPANDED-NEXT:   `-GenericParamScope {{.*}}, [20:1 - 21:1] param 0 'T'
// CHECK-EXPANDED-NEXT:     `-GenericParamScope {{.*}}, [20:1 - 21:1] param 1 'U'
// CHECK-EXPANDED-NEXT:       `-NominalTypeBodyScope {{.*}}, [20:24 - 21:1] 'GenericS0'
// CHECK-EXPANDED-NEXT: |-AbstractFunctionDeclScope {{.*}}, [23:1 - 24:1] 'genericFunc0(t:u:i:)'
// CHECK-EXPANDED-NEXT:   `-GenericParamScope {{.*}}, [23:1 - 24:1] param 0 'T'
// CHECK-EXPANDED-NEXT:     `-GenericParamScope {{.*}}, [23:1 - 24:1] param 1 'U'
// CHECK-EXPANDED-NEXT:       |-ParameterListScope {{.*}}, [23:24 - 23:48] 
// CHECK-EXPANDED-NEXT:         `-DefaultArgumentInitializerScope {{.*}}, [23:46 - 23:46] 
// CHECK-EXPANDED-NEXT:       `-FunctionBodyScope {{.*}}, [23:50 - 24:1] 
// CHECK-EXPANDED-NEXT: |-NominalTypeDeclScope {{.*}}, [26:1 - 32:1] 'ContainsGenerics0'
// CHECK-EXPANDED-NEXT:   `-NominalTypeBodyScope {{.*}}, [26:25 - 32:1] 'ContainsGenerics0'
// CHECK-EXPANDED-NEXT:     |-AbstractFunctionDeclScope {{.*}}, [27:3 - 28:3] 'init(t:u:)'
// CHECK-EXPANDED-NEXT:       `-GenericParamScope {{.*}}, [27:3 - 28:3] param 0 'T'
// CHECK-EXPANDED-NEXT:         `-GenericParamScope {{.*}}, [27:3 - 28:3] param 1 'U'
// CHECK-EXPANDED-NEXT:           |-ParameterListScope {{.*}}, [27:13 - 27:24] 
// CHECK-EXPANDED-NEXT:           `-FunctionBodyScope {{.*}}, [27:26 - 28:3] 
// CHECK-EXPANDED-NEXT:     `-AbstractFunctionDeclScope {{.*}}, [30:3 - 31:3] 'deinit'
// CHECK-EXPANDED-NEXT:       `-FunctionBodyScope {{.*}}, [30:10 - 31:3] 
// CHECK-EXPANDED-NEXT: |-TypeAliasDeclScope {{.*}}, [34:1 - 34:32] <no extended nominal?!>
// CHECK-EXPANDED-NEXT:   `-GenericParamScope {{.*}}, [34:1 - 34:32] param 0 'T'
// CHECK-EXPANDED-NEXT: |-NominalTypeDeclScope {{.*}}, [39:1 - 39:26] 'OtherArchStruct'
// CHECK-EXPANDED-NEXT:   `-NominalTypeBodyScope {{.*}}, [39:24 - 39:26] 'OtherArchStruct'
// CHECK-EXPANDED-NEXT: |-AbstractFunctionDeclScope {{.*}}, [42:1 - 101:1] 'functionBodies1(a:b:)'
// CHECK-EXPANDED-NEXT:   |-ParameterListScope {{.*}}, [42:21 - 42:37] 
// CHECK-EXPANDED-NEXT:   `-FunctionBodyScope {{.*}}, [42:39 - 101:1] 
// CHECK-EXPANDED-NEXT:     `-BraceStmtScope {{.*}}, [42:39 - 101:1] 
// CHECK-EXPANDED-NEXT:       `-PatternEntryDeclScope {{.*}}, [43:7 - 101:1] entry 0 'x1' 'x2'
// CHECK-EXPANDED-NEXT:         |-PatternEntryInitializerScope {{.*}}, [43:18 - 43:23] entry 0 'x1' 'x2'
// CHECK-EXPANDED-NEXT:         `-PatternEntryDeclScope {{.*}}, [44:7 - 101:1] entry 1 'y1' 'y2'
// CHECK-EXPANDED-NEXT:           |-PatternEntryInitializerScope {{.*}}, [44:18 - 44:23] entry 1 'y1' 'y2'
// CHECK-EXPANDED-NEXT:           `-PatternEntryDeclScope {{.*}}, [45:7 - 101:1] entry 0 'z1' 'z2'
// CHECK-EXPANDED-NEXT:             |-PatternEntryInitializerScope {{.*}}, [45:18 - 45:23] entry 0 'z1' 'z2'
// CHECK-EXPANDED-NEXT:             |-DoStmtScope {{.*}}, [46:3 - 53:3] 
// CHECK-EXPANDED-NEXT:               `-BraceStmtScope {{.*}}, [46:6 - 53:3] 
// CHECK-EXPANDED-NEXT:                 `-PatternEntryDeclScope {{.*}}, [47:9 - 53:3] entry 0 'a1'
// CHECK-EXPANDED-NEXT:                   |-PatternEntryInitializerScope {{.*}}, [47:14 - 47:14] entry 0 'a1'
// CHECK-EXPANDED-NEXT:                   `-PatternEntryDeclScope {{.*}}, [48:9 - 53:3] entry 0 'a2'
// CHECK-EXPANDED-NEXT:                     |-PatternEntryInitializerScope {{.*}}, [48:14 - 48:14] entry 0 'a2'
// CHECK-EXPANDED-NEXT:                     `-DoStmtScope {{.*}}, [49:5 - 52:5] 
// CHECK-EXPANDED-NEXT:                       `-BraceStmtScope {{.*}}, [49:8 - 52:5] 
// CHECK-EXPANDED-NEXT:                         `-PatternEntryDeclScope {{.*}}, [50:11 - 52:5] entry 0 'b1'
// CHECK-EXPANDED-NEXT:                           |-PatternEntryInitializerScope {{.*}}, [50:16 - 50:16] entry 0 'b1'
// CHECK-EXPANDED-NEXT:                           `-PatternEntryDeclScope {{.*}}, [51:11 - 52:5] entry 0 'b2'
// CHECK-EXPANDED-NEXT:                             `-PatternEntryInitializerScope {{.*}}, [51:16 - 51:16] entry 0 'b2'
// CHECK-EXPANDED-NEXT:             |-DoStmtScope {{.*}}, [54:3 - 57:3] 
// CHECK-EXPANDED-NEXT:               `-BraceStmtScope {{.*}}, [54:6 - 57:3] 
// CHECK-EXPANDED-NEXT:                 `-PatternEntryDeclScope {{.*}}, [55:9 - 57:3] entry 0 'b1'
// CHECK-EXPANDED-NEXT:                   |-PatternEntryInitializerScope {{.*}}, [55:14 - 55:14] entry 0 'b1'
// CHECK-EXPANDED-NEXT:                   `-PatternEntryDeclScope {{.*}}, [56:9 - 57:3] entry 0 'b2'
// CHECK-EXPANDED-NEXT:                     `-PatternEntryInitializerScope {{.*}}, [56:14 - 56:14] entry 0 'b2'
// CHECK-EXPANDED-NEXT:             |-AbstractFunctionDeclScope {{.*}}, [58:3 - 58:38] 'f(_:)'
// CHECK-EXPANDED-NEXT:               |-ParameterListScope {{.*}}, [58:9 - 58:18] 
// CHECK-EXPANDED-NEXT:               `-FunctionBodyScope {{.*}}, [58:27 - 58:38] 
// CHECK-EXPANDED-NEXT:                 `-BraceStmtScope {{.*}}, [58:27 - 58:38] 
// CHECK-EXPANDED-NEXT:             `-PatternEntryDeclScope {{.*}}, [59:7 - 101:1] entry 0 'f2'
// CHECK-EXPANDED-NEXT:               |-PatternEntryInitializerScope {{.*}}, [59:12 - 59:16] entry 0 'f2'
// CHECK-EXPANDED-NEXT:               |-NominalTypeDeclScope {{.*}}, [60:3 - 60:15] 'S7'
// CHECK-EXPANDED-NEXT:                 `-NominalTypeBodyScope {{.*}}, [60:13 - 60:15] 'S7'
// CHECK-EXPANDED-NEXT:               |-TypeAliasDeclScope {{.*}}, [61:3 - 61:23] <no extended nominal?!>
// CHECK-EXPANDED-NEXT:               |-IfStmtScope {{.*}}, [63:3 - 67:3] 
// CHECK-EXPANDED-NEXT:                 |-ConditionalClausePatternUseScope, [63:15 - 65:3] let b1
// CHECK-EXPANDED-NEXT:                   |-ConditionalClauseInitializerScope, [63:15 - 63:15] 
// CHECK-EXPANDED-NEXT:                   `-ConditionalClausePatternUseScope, [63:27 - 65:3] let b2
// CHECK-EXPANDED-NEXT:                     |-ConditionalClauseInitializerScope, [63:27 - 63:27] 
// CHECK-EXPANDED-NEXT:                     `-BraceStmtScope {{.*}}, [63:29 - 65:3] 
// CHECK-EXPANDED-NEXT:                       `-PatternEntryDeclScope {{.*}}, [64:9 - 65:3] entry 0 'c1'
// CHECK-EXPANDED-NEXT:                         `-PatternEntryInitializerScope {{.*}}, [64:14 - 64:14] entry 0 'c1'
// CHECK-EXPANDED-NEXT:                 `-BraceStmtScope {{.*}}, [65:10 - 67:3] 
// CHECK-EXPANDED-NEXT:                   `-PatternEntryDeclScope {{.*}}, [66:9 - 67:3] entry 0 'c2'
// CHECK-EXPANDED-NEXT:                     `-PatternEntryInitializerScope {{.*}}, [66:14 - 66:14] entry 0 'c2'
// CHECK-EXPANDED-NEXT:               `-GuardStmtScope {{.*}}, [69:3 - 101:1] 
// CHECK-EXPANDED-NEXT:                 `-ConditionalClausePatternUseScope, [69:18 - 101:1] let b1
// CHECK-EXPANDED-NEXT:                   |-ConditionalClauseInitializerScope, [69:18 - 69:18] 
// CHECK-EXPANDED-NEXT:                   |-ClosureParametersScope {{.*}}, [69:21 - 69:30] 
// CHECK-EXPANDED-NEXT:                     `-BraceStmtScope {{.*}}, [69:21 - 69:30] 
// CHECK-EXPANDED-NEXT:                   `-ConditionalClausePatternUseScope, [69:46 - 101:1] let b2
// CHECK-EXPANDED-NEXT:                     |-ConditionalClauseInitializerScope, [69:46 - 69:46] 
// CHECK-EXPANDED-NEXT:                     |-GuardStmtBodyScope, [69:53 - 72:3] 
// CHECK-EXPANDED-NEXT:                       `-BraceStmtScope {{.*}}, [69:53 - 72:3] 
// CHECK-EXPANDED-NEXT:                         `-PatternEntryDeclScope {{.*}}, [70:9 - 72:3] entry 0 'c'
// CHECK-EXPANDED-NEXT:                           `-PatternEntryInitializerScope {{.*}}, [70:13 - 70:13] entry 0 'c'
// CHECK-EXPANDED-NEXT:                     |-WhileStmtScope {{.*}}, [74:3 - 76:3] 
// CHECK-EXPANDED-NEXT:                       `-ConditionalClausePatternUseScope, [74:18 - 76:3] let b3
// CHECK-EXPANDED-NEXT:                         |-ConditionalClauseInitializerScope, [74:18 - 74:18] 
// CHECK-EXPANDED-NEXT:                         `-ConditionalClausePatternUseScope, [74:30 - 76:3] let b4
// CHECK-EXPANDED-NEXT:                           |-ConditionalClauseInitializerScope, [74:30 - 74:30] 
// CHECK-EXPANDED-NEXT:                           `-BraceStmtScope {{.*}}, [74:32 - 76:3] 
// CHECK-EXPANDED-NEXT:                             `-PatternEntryDeclScope {{.*}}, [75:9 - 76:3] entry 0 'c'
// CHECK-EXPANDED-NEXT:                               `-PatternEntryInitializerScope {{.*}}, [75:13 - 75:13] entry 0 'c'
// CHECK-EXPANDED-NEXT:                     |-RepeatWhileScope {{.*}}, [78:3 - 78:20] 
// CHECK-EXPANDED-NEXT:                     |-ForEachStmtScope {{.*}}, [80:3 - 82:3] 
// CHECK-EXPANDED-NEXT:                       `-ForEachPatternScope, [80:52 - 82:3] 
// CHECK-EXPANDED-NEXT:                     |-DoCatchStmtScope {{.*}}, [84:3 - 88:3] 
// CHECK-EXPANDED-NEXT:                       |-BraceStmtScope {{.*}}, [84:6 - 86:3] 
// CHECK-EXPANDED-NEXT:                       |-CaseStmtScope {{.*}}, [86:5 - 87:3] 
// CHECK-EXPANDED-NEXT:                         `-CaseLabelItemScope, [86:37 - 86:51] 
// CHECK-EXPANDED-NEXT:                       `-CaseStmtScope {{.*}}, [87:5 - 88:3] 
// CHECK-EXPANDED-NEXT:                     |-SwitchStmtScope {{.*}}, [90:3 - 99:3] 
// CHECK-EXPANDED-NEXT:                       |-CaseStmtScope {{.*}}, [91:3 - 92:10] 
// CHECK-EXPANDED-NEXT:                         |-CaseLabelItemScope, [91:29 - 91:34] 
// CHECK-EXPANDED-NEXT:                         `-CaseStmtBodyScope, [92:5 - 92:10] 
// CHECK-EXPANDED-NEXT:                           `-BraceStmtScope {{.*}}, [92:5 - 92:10] 
// CHECK-EXPANDED-NEXT:                       |-CaseStmtScope {{.*}}, [94:3 - 95:10] 
// CHECK-EXPANDED-NEXT:                         `-CaseStmtBodyScope, [95:5 - 95:10] 
// CHECK-EXPANDED-NEXT:                           `-BraceStmtScope {{.*}}, [95:5 - 95:10] 
// CHECK-EXPANDED-NEXT:                       `-CaseStmtScope {{.*}}, [97:3 - 98:10] 
// CHECK-EXPANDED-NEXT:                         `-CaseStmtBodyScope, [98:5 - 98:10] 
// CHECK-EXPANDED-NEXT:                           `-BraceStmtScope {{.*}}, [98:5 - 98:10] 
// CHECK-EXPANDED-NEXT:                     `-ForEachStmtScope {{.*}}, [100:3 - 100:38] 
// CHECK-EXPANDED-NEXT:                       `-ForEachPatternScope, [100:36 - 100:38] 
// CHECK-EXPANDED-NEXT: |-AbstractFunctionDeclScope {{.*}}, [103:1 - 103:26] 'throwing()'
// CHECK-EXPANDED-NEXT:   `-FunctionBodyScope {{.*}}, [103:24 - 103:26] 
// CHECK-EXPANDED-NEXT: |-NominalTypeDeclScope {{.*}}, [105:1 - 107:1] 'MyError'
// CHECK-EXPANDED-NEXT:   `-NominalTypeBodyScope {{.*}}, [105:24 - 107:1] 'MyError'
// CHECK-EXPANDED-NEXT:     `-PatternEntryDeclScope {{.*}}, [106:7 - 106:14] entry 0 'value'
// CHECK-EXPANDED-NEXT: |-NominalTypeDeclScope {{.*}}, [109:1 - 113:1] 'MyEnum'
// CHECK-EXPANDED-NEXT:   `-NominalTypeBodyScope {{.*}}, [109:13 - 113:1] 'MyEnum'
// CHECK-EXPANDED-NEXT:     |-EnumElementScope {{.*}}, [110:8 - 110:8] 
// CHECK-EXPANDED-NEXT:     |-EnumElementScope {{.*}}, [111:8 - 111:18] 
// CHECK-EXPANDED-NEXT:       `-ParameterListScope {{.*}}, [111:14 - 111:18] 
// CHECK-EXPANDED-NEXT:     `-EnumElementScope {{.*}}, [112:8 - 112:8] 
// CHECK-EXPANDED-NEXT: |-NominalTypeDeclScope {{.*}}, [115:1 - 131:1] 'StructContainsAbstractStorageDecls'
// CHECK-EXPANDED-NEXT:   `-NominalTypeBodyScope {{.*}}, [115:43 - 131:1] 'StructContainsAbstractStorageDecls'
// CHECK-EXPANDED-NEXT:     |-SubscriptDeclScope {{.*}}, [116:3 - 122:3] main.(file).StructContainsAbstractStorageDecls.subscript(_:_:)@{{.*}}scope_map-astscopelookup.swift:116:3
// CHECK-EXPANDED-NEXT:       |-ParameterListScope {{.*}}, [116:13 - 116:28] 
// CHECK-EXPANDED-NEXT:       |-AbstractFunctionDeclScope {{.*}}, [117:5 - 118:5] '_'
// CHECK-EXPANDED-NEXT:         `-FunctionBodyScope {{.*}}, [117:9 - 118:5] 
// CHECK-EXPANDED-NEXT:       `-AbstractFunctionDeclScope {{.*}}, [119:5 - 121:5] '_'
// CHECK-EXPANDED-NEXT:         `-FunctionBodyScope {{.*}}, [119:9 - 121:5] 
// CHECK-EXPANDED-NEXT:           `-BraceStmtScope {{.*}}, [119:9 - 121:5] 
// CHECK-EXPANDED-NEXT:     `-PatternEntryDeclScope {{.*}}, [124:7 - 130:3] entry 0 'computed'
// CHECK-EXPANDED-NEXT:       |-AbstractFunctionDeclScope {{.*}}, [125:5 - 127:5] '_'
// CHECK-EXPANDED-NEXT:         `-FunctionBodyScope {{.*}}, [125:9 - 127:5] 
// CHECK-EXPANDED-NEXT:           `-BraceStmtScope {{.*}}, [125:9 - 127:5] 
// CHECK-EXPANDED-NEXT:       `-AbstractFunctionDeclScope {{.*}}, [128:5 - 129:5] '_'
// CHECK-EXPANDED-NEXT:         `-FunctionBodyScope {{.*}}, [128:9 - 129:5] 
// CHECK-EXPANDED-NEXT: |-NominalTypeDeclScope {{.*}}, [133:1 - 141:1] 'ClassWithComputedProperties'
// CHECK-EXPANDED-NEXT:   `-NominalTypeBodyScope {{.*}}, [133:35 - 141:1] 'ClassWithComputedProperties'
// CHECK-EXPANDED-NEXT:     |-PatternEntryDeclScope {{.*}}, [134:7 - 136:3] entry 0 'willSetProperty'
// CHECK-EXPANDED-NEXT:       |-PatternEntryInitializerScope {{.*}}, [134:30 - 134:30] entry 0 'willSetProperty'
// CHECK-EXPANDED-NEXT:       `-AbstractFunctionDeclScope {{.*}}, [135:5 - 135:15] '_'
// CHECK-EXPANDED-NEXT:         `-FunctionBodyScope {{.*}}, [135:13 - 135:15] 
// CHECK-EXPANDED-NEXT:     `-PatternEntryDeclScope {{.*}}, [138:7 - 140:3] entry 0 'didSetProperty'
// CHECK-EXPANDED-NEXT:       |-PatternEntryInitializerScope {{.*}}, [138:29 - 138:29] entry 0 'didSetProperty'
// CHECK-EXPANDED-NEXT:       `-AbstractFunctionDeclScope {{.*}}, [139:5 - 139:14] '_'
// CHECK-EXPANDED-NEXT:         `-FunctionBodyScope {{.*}}, [139:12 - 139:14] 
// CHECK-EXPANDED-NEXT: |-AbstractFunctionDeclScope {{.*}}, [143:1 - 156:1] 'funcWithComputedProperties(i:)'
// CHECK-EXPANDED-NEXT:   |-ParameterListScope {{.*}}, [143:32 - 143:39] 
// CHECK-EXPANDED-NEXT:   `-FunctionBodyScope {{.*}}, [143:41 - 156:1] 
// CHECK-EXPANDED-NEXT:     `-BraceStmtScope {{.*}}, [143:41 - 156:1] 
// CHECK-EXPANDED-NEXT:       `-PatternEntryDeclScope {{.*}}, [144:7 - 156:1] entry 0 'computed'
// CHECK-EXPANDED-NEXT:         |-AbstractFunctionDeclScope {{.*}}, [145:5 - 146:5] '_'
// CHECK-EXPANDED-NEXT:           `-FunctionBodyScope {{.*}}, [145:9 - 146:5] 
// CHECK-EXPANDED-NEXT:         |-AbstractFunctionDeclScope {{.*}}, [147:5 - 149:5] '_'
// CHECK-EXPANDED-NEXT:           `-FunctionBodyScope {{.*}}, [147:9 - 149:5] 
// CHECK-EXPANDED-NEXT:             `-BraceStmtScope {{.*}}, [147:9 - 149:5] 
// CHECK-EXPANDED-NEXT:         `-PatternEntryDeclScope {{.*}}, [150:6 - 156:1] entry 1 'stored1' 'stored2'
// CHECK-EXPANDED-NEXT:           |-PatternEntryInitializerScope {{.*}}, [150:31 - 150:36] entry 1 'stored1' 'stored2'
// CHECK-EXPANDED-NEXT:           `-PatternEntryDeclScope {{.*}}, [151:3 - 156:1] entry 2 'alsoComputed'
// CHECK-EXPANDED-NEXT:             |-AbstractFunctionDeclScope {{.*}}, [151:25 - 153:3] '_'
// CHECK-EXPANDED-NEXT:               `-FunctionBodyScope {{.*}}, [151:25 - 153:3] 
// CHECK-EXPANDED-NEXT:                 `-BraceStmtScope {{.*}}, [151:25 - 153:3] 
// CHECK-EXPANDED-NEXT:             `-DoStmtScope {{.*}}, [155:3 - 155:8] 
// CHECK-EXPANDED-NEXT: |-AbstractFunctionDeclScope {{.*}}, [158:1 - 163:1] 'closures()'
// CHECK-EXPANDED-NEXT:   `-FunctionBodyScope {{.*}}, [158:17 - 163:1] 
// CHECK-EXPANDED-NEXT:     `-BraceStmtScope {{.*}}, [158:17 - 163:1] 
// CHECK-EXPANDED-NEXT:       |-ClosureParametersScope {{.*}}, [159:10 - 161:3] 
// CHECK-EXPANDED-NEXT:         `-BraceStmtScope {{.*}}, [159:10 - 161:3] 
// CHECK-EXPANDED-NEXT:           `-ClosureParametersScope {{.*}}, [160:12 - 160:22] 
// CHECK-EXPANDED-NEXT:             `-BraceStmtScope {{.*}}, [160:12 - 160:22] 
// CHECK-EXPANDED-NEXT:       `-ClosureParametersScope {{.*}}, [162:10 - 162:19] 
// CHECK-EXPANDED-NEXT:         `-BraceStmtScope {{.*}}, [162:10 - 162:19] 
// CHECK-EXPANDED-NEXT: `-TopLevelCodeScope {{.*}}, [165:1 - 480:1] 
// CHECK-EXPANDED-NEXT:   `-BraceStmtScope {{.*}}, [165:1 - 480:1] 
// CHECK-EXPANDED-NEXT:     |-ClosureParametersScope {{.*}}, [165:1 - 165:14] 
// CHECK-EXPANDED-NEXT:       `-BraceStmtScope {{.*}}, [165:1 - 165:14] 
// CHECK-EXPANDED-NEXT:     |-AbstractFunctionDeclScope {{.*}}, [167:1 - 176:1] 'defaultArguments(i:j:)'
// CHECK-EXPANDED-NEXT:       |-ParameterListScope {{.*}}, [167:22 - 168:49] 
// CHECK-EXPANDED-NEXT:         |-DefaultArgumentInitializerScope {{.*}}, [167:32 - 167:32] 
// CHECK-EXPANDED-NEXT:         `-DefaultArgumentInitializerScope {{.*}}, [168:32 - 168:48] 
// CHECK-EXPANDED-NEXT:           `-ClosureParametersScope {{.*}}, [168:32 - 168:42] 
// CHECK-EXPANDED-NEXT:             `-BraceStmtScope {{.*}}, [168:32 - 168:42] 
// CHECK-EXPANDED-NEXT:       `-FunctionBodyScope {{.*}}, [168:51 - 176:1] 
// CHECK-EXPANDED-NEXT:         `-BraceStmtScope {{.*}}, [168:51 - 176:1] 
// CHECK-EXPANDED-NEXT:           |-AbstractFunctionDeclScope {{.*}}, [170:3 - 172:3] 'localWithDefaults(i:j:)'
// CHECK-EXPANDED-NEXT:             |-ParameterListScope {{.*}}, [170:25 - 171:52] 
// CHECK-EXPANDED-NEXT:               |-DefaultArgumentInitializerScope {{.*}}, [170:35 - 170:35] 
// CHECK-EXPANDED-NEXT:               `-DefaultArgumentInitializerScope {{.*}}, [171:35 - 171:51] 
// CHECK-EXPANDED-NEXT:                 `-ClosureParametersScope {{.*}}, [171:35 - 171:45] 
// CHECK-EXPANDED-NEXT:                   `-BraceStmtScope {{.*}}, [171:35 - 171:45] 
// CHECK-EXPANDED-NEXT:             `-FunctionBodyScope {{.*}}, [171:54 - 172:3] 
// CHECK-EXPANDED-NEXT:           `-PatternEntryDeclScope {{.*}}, [174:7 - 176:1] entry 0 'a'
// CHECK-EXPANDED-NEXT:             `-PatternEntryInitializerScope {{.*}}, [174:11 - 175:11] entry 0 'a'
// CHECK-EXPANDED-NEXT:               `-ClosureParametersScope {{.*}}, [175:3 - 175:8] 
// CHECK-EXPANDED-NEXT:                 `-BraceStmtScope {{.*}}, [175:3 - 175:8] 
// CHECK-EXPANDED-NEXT:     |-NominalTypeDeclScope {{.*}}, [178:1 - 181:1] 'PatternInitializers'
// CHECK-EXPANDED-NEXT:       `-NominalTypeBodyScope {{.*}}, [178:28 - 181:1] 'PatternInitializers'
// CHECK-EXPANDED-NEXT:         |-PatternEntryDeclScope {{.*}}, [179:7 - 179:21] entry 0 'a' 'b'
// CHECK-EXPANDED-NEXT:           `-PatternEntryInitializerScope {{.*}}, [179:16 - 179:21] entry 0 'a' 'b'
// CHECK-EXPANDED-NEXT:         `-PatternEntryDeclScope {{.*}}, [180:7 - 180:25] entry 1 'c' 'd'
// CHECK-EXPANDED-NEXT:           `-PatternEntryInitializerScope {{.*}}, [180:16 - 180:25] entry 1 'c' 'd'
// CHECK-EXPANDED-NEXT:     |-NominalTypeDeclScope {{.*}}, [183:1 - 185:1] 'ProtoWithSubscript'
// CHECK-EXPANDED-NEXT:       `-GenericParamScope {{.*}}, [183:1 - 185:1] param 0 'Self : ProtoWithSubscript'
// CHECK-EXPANDED-NEXT:         `-NominalTypeBodyScope {{.*}}, [183:29 - 185:1] 'ProtoWithSubscript'
// CHECK-EXPANDED-NEXT:           `-SubscriptDeclScope {{.*}}, [184:3 - 184:43] main.(file).ProtoWithSubscript.subscript(_:)@{{.*}}scope_map-astscopelookup.swift:184:3
// CHECK-EXPANDED-NEXT:             |-ParameterListScope {{.*}}, [184:12 - 184:24] 
// CHECK-EXPANDED-NEXT:             |-AbstractFunctionDeclScope {{.*}}, [184:35 - 184:35] '_'
// CHECK-EXPANDED-NEXT:             `-AbstractFunctionDeclScope {{.*}}, [184:39 - 184:39] '_'
// CHECK-EXPANDED-NEXT:     |-AbstractFunctionDeclScope {{.*}}, [187:1 - 189:1] 'localPatternsWithSharedType()'
// CHECK-EXPANDED-NEXT:       `-FunctionBodyScope {{.*}}, [187:36 - 189:1] 
// CHECK-EXPANDED-NEXT:         `-BraceStmtScope {{.*}}, [187:36 - 189:1] 
// CHECK-EXPANDED-NEXT:           `-PatternEntryDeclScope {{.*}}, [188:7 - 189:1] entry 0 'i'
// CHECK-EXPANDED-NEXT:             `-PatternEntryDeclScope {{.*}}, [188:10 - 189:1] entry 1 'j'
// CHECK-EXPANDED-NEXT:               `-PatternEntryDeclScope {{.*}}, [188:13 - 189:1] entry 2 'k'
// CHECK-EXPANDED-NEXT:     |-NominalTypeDeclScope {{.*}}, [191:1 - 195:1] 'LazyProperties'
// CHECK-EXPANDED-NEXT:       `-NominalTypeBodyScope {{.*}}, [191:22 - 195:1] 'LazyProperties'
// CHECK-EXPANDED-NEXT:         |-PatternEntryDeclScope {{.*}}, [192:7 - 192:20] entry 0 'value'
// CHECK-EXPANDED-NEXT:           `-PatternEntryInitializerScope {{.*}}, [192:20 - 192:20] entry 0 'value'
// CHECK-EXPANDED-NEXT:         `-PatternEntryDeclScope {{.*}}, [194:12 - 194:29] entry 0 'prop'
// CHECK-EXPANDED-NEXT:           `-PatternEntryInitializerScope {{.*}}, [194:24 - 194:29] entry 0 'prop'
// CHECK-EXPANDED-NEXT:     `-AbstractFunctionDeclScope {{.*}}, [197:1 - 206:1] 'HasLabeledDo()'
// CHECK-EXPANDED-NEXT:       `-FunctionBodyScope {{.*}}, [197:21 - 206:1] 
// CHECK-EXPANDED-NEXT:         `-BraceStmtScope {{.*}}, [197:21 - 206:1] 
// CHECK-EXPANDED-NEXT:           `-DoStmtScope {{.*}}, [198:3 - 205:3] 
// CHECK-EXPANDED-NEXT:             `-BraceStmtScope {{.*}}, [199:6 - 205:3] 
// CHECK-EXPANDED-NEXT:               `-ForEachStmtScope {{.*}}, [200:5 - 204:5] 
// CHECK-EXPANDED-NEXT:                 `-ForEachPatternScope, [200:22 - 204:5] 
// CHECK-EXPANDED-NEXT:                   `-BraceStmtScope {{.*}}, [200:22 - 204:5] 
// CHECK-EXPANDED-NEXT:                     `-IfStmtScope {{.*}}, [201:7 - 203:7] 
// CHECK-EXPANDED-NEXT:                       `-BraceStmtScope {{.*}}, [201:21 - 203:7] 


// RUN: not %target-swift-frontend -dump-scope-maps 71:8,27:20,6:18,167:32,180:18,194:26 %s 2> %t.searches
// RUN: %FileCheck -check-prefix CHECK-SEARCHES %s < %t.searches

// CHECK-SEARCHES:      ***Scope at 71:8***
// CHECK-SEARCHES-NEXT: PatternEntryDeclScope {{.*}}, [70:9 - 72:3] entry 0 'c'
// CHECK-SEARCHES-NEXT: Local bindings: c
// CHECK-SEARCHES-NEXT: ***Scope at 27:20***
// CHECK-SEARCHES-NEXT: ParameterListScope {{.*}}, [27:13 - 27:24]
// CHECK-SEARCHES-NEXT: ***Scope at 6:18***
// CHECK-SEARCHES-NEXT: NominalTypeBodyScope {{.*}}, [6:17 - 6:19] 'InnerC0'
// CHECK-SEARCHES-NEXT: ***Scope at 167:32***
// CHECK-SEARCHES-NEXT: DefaultArgumentInitializerScope {{.*}}, [167:32 - 167:32]
// CHECK-SEARCHES-NEXT: ***Scope at 180:18***
// CHECK-SEARCHES-NEXT: PatternEntryInitializerScope {{.*}}, [180:16 - 180:25] entry 1 'c' 'd'
// CHECK-SEARCHES-NEXT: ***Scope at 194:26***
// CHECK-SEARCHES-NEXT: PatternEntryInitializerScope {{.*}}, [194:24 - 194:29] entry 0 'prop'
// CHECK-SEARCHES-NEXT: Local bindings: self


// CHECK-SEARCHES-NOT:  ***Complete scope map***

