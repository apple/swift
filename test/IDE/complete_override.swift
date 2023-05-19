// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -disable-implicit-concurrency-module-import -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t
// RUN: sed -n -e '1,/NO_ERRORS_UP_TO_HERE$/ p' %s > %t_no_errors.swift
// RUN: %target-swift-frontend -typecheck -verify -disable-objc-attr-requires-foundation-module -enable-objc-interop %t_no_errors.swift

// NORESULT: Token

@objc
class TagPA {}
@objc
protocol ProtocolA {
  init(fromProtocolA: Int)

  func protoAFunc()
  @objc optional func protoAFuncOptional()

  subscript(a: TagPA) -> Int { get }

  var protoAVarRW: Int { get set }
  var protoAVarRO: Int { get }
}
// WITH_PA-DAG: Decl[Constructor]/Super:    required init(fromProtocolA: Int) {|}{{; name=.+$}}
// WITH_PA-DAG: Decl[InstanceMethod]/Super: func protoAFunc() {|}{{; name=.+$}}
// WITH_PA-DAG: Decl[InstanceMethod]/Super: func protoAFuncOptional() {|}{{; name=.+$}}
// WITH_PA-DAG: Decl[Subscript]/Super:      subscript(a: TagPA) -> Int {|}{{; name=.+$}}
// WITH_PA-DAG: Decl[InstanceVar]/Super:    var protoAVarRW: Int{{; name=.+$}}
// WITH_PA-DAG: Decl[InstanceVar]/Super:    var protoAVarRO: Int{{; name=.+$}}

// WITH_PA_NO_PROTOFUNCA-DAG: Decl[Constructor]/Super:    required init(fromProtocolA: Int) {|}{{; name=.+$}}
// WITH_PA_NO_PROTOFUNCA-DAG: Decl[InstanceMethod]/Super: func protoAFuncOptional() {|}{{; name=.+$}}
// WITH_PA_NO_PROTOFUNCA-DAG: Decl[InstanceVar]/Super:    var protoAVarRW: Int{{; name=.+$}}
// WITH_PA_NO_PROTOFUNCA-DAG: Decl[InstanceVar]/Super:    var protoAVarRO: Int{{; name=.+$}}

struct TagPB {}
protocol ProtocolB : ProtocolA {
  init(fromProtocolB: Int)

  func protoBFunc()

  subscript(a: TagPB) -> Int { get }

  var protoBVarRW: Int { get set }
  var protoBVarRO: Int { get }
}
// WITH_PB-DAG: Decl[Constructor]/Super:    required init(fromProtocolA: Int) {|}{{; name=.+$}}
// WITH_PB-DAG: Decl[InstanceMethod]/Super: func protoAFunc() {|}{{; name=.+$}}
// WITH_PB-DAG: Decl[InstanceMethod]/Super: func protoBFunc() {|}{{; name=.+$}}
// WITH_PB-DAG: Decl[Subscript]/Super:      subscript(a: TagPB) -> Int {|}{{; name=.+$}}
// WITH_PB-DAG: Decl[InstanceVar]/Super:    var protoBVarRW: Int{{; name=.+$}}
// WITH_PB-DAG: Decl[InstanceVar]/Super:    var protoBVarRO: Int{{; name=.+$}}

struct TagPE {}
protocol ProtocolE {
  init(fromProtocolE: Int)

  func protoEFunc()

  subscript(a: TagPE) -> Int { get }

  var protoEVarRW: Int { get set }
  var protoEVarRO: Int { get }
}
// WITH_PE-DAG: Decl[Constructor]/Super:    required init(fromProtocolE: Int) {|}{{; name=.+$}}
// WITH_PE-DAG: Decl[InstanceMethod]/Super: func protoEFunc() {|}{{; name=.+$}}
// WITH_PE-DAG: Decl[Subscript]/Super:      subscript(a: TagPE) -> Int {|}{{; name=.+$}}
// WITH_PE-DAG: Decl[InstanceVar]/Super:    var protoEVarRW: Int{{; name=.+$}}
// WITH_PE-DAG: Decl[InstanceVar]/Super:    var protoEVarRO: Int{{; name=.+$}}

class BaseA {
  init(fromBaseA: Int) {}
  init(fromBaseAWithParamName foo: Int, withOther bar: Double) {}
  convenience init(convenienceFromBaseA: Double) {
    self.init(fromBaseA: 0)
  }

  func baseAFunc(foo x: Int) {}
  func baseAFunc2(foo x: Int) {}

  var baseAVarRW: Int { get { return 0 } set {} }
  var baseAVarRO: Int { return 0 }
}
// WITH_BA-DAG: Decl[Constructor]/Super:    override init(fromBaseA: Int) {|}{{; name=.+$}}
// WITH_BA-DAG: Decl[Constructor]/Super:    override init(fromBaseAWithParamName foo: Int, withOther bar: Double) {|}{{; name=.+$}}
// WITH_BA-DAG: Decl[InstanceMethod]/Super: override func baseAFunc(foo x: Int) {|}{{; name=.+$}}
// WITH_BA-DAG: Decl[InstanceMethod]/Super: override func baseAFunc2(foo x: Int) {|}{{; name=.+$}}
// WITH_BA-DAG: Decl[InstanceVar]/Super:    override var baseAVarRW: Int{{; name=.+$}}
// WITH_BA-DAG: Decl[InstanceVar]/Super:    override var baseAVarRO: Int{{; name=.+$}}

class BaseB : BaseA {
  override func baseAFunc2(foo x: Int) {}

  init(fromBaseB: Int) {}
  convenience init(convenienceFromBaseB: Double) {
    self.init(fromBaseB: 0)
  }

  func baseBFunc() {}

  var baseBVarRW: Int { get { return 0 } set {} }
  var baseBVarRO: Int { return 0 }
}
// WITH_BB-DAG: Decl[InstanceMethod]/Super: override func baseAFunc(foo x: Int) {|}{{; name=.+$}}
// WITH_BB-DAG: Decl[InstanceMethod]/Super: override func baseAFunc2(foo x: Int) {|}{{; name=.+$}}
// WITH_BB-DAG: Decl[Constructor]/Super:    override init(fromBaseB: Int) {|}{{; name=.+$}}
// WITH_BB-DAG: Decl[InstanceMethod]/Super: override func baseBFunc() {|}{{; name=.+$}}
// WITH_BB-DAG: Decl[InstanceVar]/Super:    override var baseAVarRW: Int{{; name=.+$}}
// WITH_BB-DAG: Decl[InstanceVar]/Super:    override var baseAVarRO: Int{{; name=.+$}}
// WITH_BB-DAG: Decl[InstanceVar]/Super:    override var baseBVarRW: Int{{; name=.+$}}
// WITH_BB-DAG: Decl[InstanceVar]/Super:    override var baseBVarRO: Int{{; name=.+$}}

class BaseE : ProtocolE {
  required init(fromProtocolE: Int) {}

  func protoEFunc() {}

  subscript(a: TagPE) -> Int { return 0 }

  var protoEVarRW: Int { get { return 0 } set {} }
  var protoEVarRO: Int { return 0 }

  init(fromBaseE: Int) {}

  func baseEFunc() {}

  var baseEVarRW: Int { get { return 0 } set {} }
  var baseEVarRO: Int { return 0 }
}
// WITH_BE-DAG: Decl[Constructor]/Super:    required init(fromProtocolE: Int) {|}{{; name=.+$}}
// WITH_BE-DAG: Decl[InstanceMethod]/Super: override func protoEFunc() {|}{{; name=.+$}}
// WITH_BE-DAG: Decl[Subscript]/Super:      override subscript(a: TagPE) -> Int {|}{{; name=.+$}}
// WITH_BE-DAG: Decl[Constructor]/Super:    override init(fromBaseE: Int) {|}{{; name=.+$}}
// WITH_BE-DAG: Decl[InstanceMethod]/Super: override func baseEFunc() {|}{{; name=.+$}}
// WITH_BE-DAG: Decl[InstanceVar]/Super:    override var protoEVarRW: Int{{; name=.+$}}
// WITH_BE-DAG: Decl[InstanceVar]/Super:    override var protoEVarRO: Int{{; name=.+$}}
// WITH_BE-DAG: Decl[InstanceVar]/Super:    override var baseEVarRW: Int{{; name=.+$}}
// WITH_BE-DAG: Decl[InstanceVar]/Super:    override var baseEVarRO: Int{{; name=.+$}}

class ProtocolEImpl /* : ProtocolE but does not implement the protocol */ {
  init(fromProtocolE: Int) {}

  func protoEFunc() {}

  subscript(a: TagPE) -> Int { return 0 }

  var protoEVarRW: Int { get { return 0 } set {} }
  var protoEVarRO: Int { return 0 }
}
// WITH_PEI-DAG: Decl[Constructor]/Super:    override init(fromProtocolE: Int) {|}{{; name=.+$}}
// WITH_PEI-DAG: Decl[InstanceMethod]/Super: override func protoEFunc() {|}{{; name=.+$}}
// WITH_PEI-DAG: Decl[Subscript]/Super:      override subscript(a: TagPE) -> Int {|}{{; name=.+$}}
// WITH_PEI-DAG: Decl[InstanceVar]/Super:    override var protoEVarRW: Int{{; name=.+$}}
// WITH_PEI-DAG: Decl[InstanceVar]/Super:    override var protoEVarRO: Int{{; name=.+$}}

// NO_ERRORS_UP_TO_HERE

class TestClass_PA : ProtocolA {
  func ERROR() {}

  #^CLASS_PA?check=CLASS_PA;check=WITH_PA;keywords=false^#
}
// CLASS_PA: Begin completions, 6 items

class TestClass_PA_Ext {
  func ERROR1() {}
  #^CLASS_PA_EXT_1?check=CLASS_PA;check=WITH_PA;keywords=false^#
}
extension TestClass_PA_Ext : ProtocolA {
  func ERROR2() {}
  #^CLASS_PA_EXT_2?check=CLASS_PA;check=WITH_PA;keywords=false^#
}

class TestClass_PB : ProtocolB {
  #^CLASS_PB?check=CLASS_PB;check=WITH_PB;keywords=false^#
}
// CLASS_PB: Begin completions, 11 items

class TestClass_PA_PB : ProtocolA, ProtocolB {
  #^CLASS_PA_PB?check=CLASS_PA_PB;check=WITH_PA;check=WITH_PB;keywords=false^#
}
// CLASS_PA_PB: Begin completions, 11 items

class TestClass_BA : BaseA {
  #^CLASS_BA?check=CLASS_BA;check=WITH_BA;keywords=false^#
}
// CLASS_BA: Begin completions, 6 items

class TestClass_BA_PA : BaseA, ProtocolA {
  #^CLASS_BA_PA?check=CLASS_BA_PA;check=WITH_BA;check=WITH_PA;keywords=false^#
}
// CLASS_BA_PA: Begin completions, 12 items

class TestClass_BA_PA_Ext : BaseA {
  #^CLASS_BA_PA_EXT1?check=CLASS_BA_PA;check=WITH_BA;check=WITH_PA;keywords=false^#
}

extension TestClass_BA_PA_Ext : ProtocolA {
  #^CLASS_BA_PA_EXT2?check=CLASS_BA_PA;check=WITH_BA;check=WITH_PA;keywords=false^#
}

class TestClass_BA_PB : BaseA, ProtocolB {
  #^CLASS_BA_PB?check=CLASS_BA_PB;check=WITH_BA;check=WITH_PB;keywords=false^#
}
// CLASS_BA_PB: Begin completions, 17 items

class TestClass_BB : BaseB {
  #^CLASS_BB?check=CLASS_BB;check=WITH_BB;keywords=false^#
}
// CLASS_BB: Begin completions, 8 items

class TestClass_BE : BaseE {
  #^CLASS_BE?check=CLASS_BE;check=WITH_BE;keywords=false^#
}
// CLASS_BE: Begin completions, 9 items

class TestClass_BE_PA : BaseE, ProtocolA {
  #^CLASS_BE_PA?check=CLASS_BE_PA;check=WITH_BE;check=WITH_PA;keywords=false^#
}
// CLASS_BE_PA: Begin completions, 15 items

class TestClass_BE_PA_PE : BaseE, ProtocolA, ProtocolE {
  #^CLASS_BE_PA_PE?check=CLASS_BE_PA_PE;check=WITH_BE;check=WITH_PA;keywords=false^#
}
// CLASS_BE_PA_PE: Begin completions, 15 items

class TestClass_BE_PA_PE_Ext : BaseE {
  #^CLASS_BE_PA_PE_EXT1?check=CLASS_BE_PA_PE;check=WITH_BE;check=WITH_PA;keywords=false^#
}
extension TestClass_BE_PA_PE_Ext : ProtocolA, ProtocolE {
  #^CLASS_BE_PA_PE_EXT2?check=CLASS_BE_PA_PE;check=WITH_BE;check=WITH_PA;keywords=false^#
}

class TestClass_PEI_PE : ProtocolEImpl, ProtocolE {
  #^CLASS_PEI_PE?check=CLASS_PEI_PE;check=WITH_PEI;keywords=false^#
}
// CLASS_PEI_PE: Begin completions, 5 items

protocol TestProtocol_PA : ProtocolA {
  #^PROTOCOL_PA?keywords=false;check=NORESULT^#
}

extension TestProtocol_PA {
  #^PROTOCOL_PA_EXT?keywords=false^#
}

// PROTOCOL_PA_EXT-DAG: Decl[Constructor]/Super:            init(fromProtocolA: Int) {|}; name=init(fromProtocolA:)
// PROTOCOL_PA_EXT-DAG: Decl[InstanceMethod]/Super:         func protoAFunc() {|}; name=protoAFunc()
// PROTOCOL_PA_EXT-DAG: Decl[InstanceMethod]/Super:         func protoAFuncOptional() {|}; name=protoAFuncOptional()
// PROTOCOL_PA_EXT-DAG: Decl[Subscript]/Super:              subscript(a: TagPA) -> Int {|}; name=subscript(:)
// PROTOCOL_PA_EXT-DAG: Decl[InstanceVar]/Super:            var protoAVarRW: Int; name=protoAVarRW
// PROTOCOL_PA_EXT-DAG: Decl[InstanceVar]/Super:            var protoAVarRO: Int; name=protoAVarRO

class OuterNominal : ProtocolA {
  class Inner {
    #^NESTED_NOMINAL?check=NORESULT;keywords=false^#
  }
}

class OuterNominal2: ProtocolA {
  var f = { #^NESTED_CLOSURE_1?keywords=false^# }()
}
// NESTED_CLOSURE_1-NOT: Decl{{.*}}/Super: func
// NESTED_CLOSURE_1-NOT: {|}

class OuterNominal3: ProtocolA {
  var f = { static #^NESTED_CLOSURE_2?keywords=false^# }()
}
// NESTED_CLOSURE_2-NOT: Decl{{.*}}/Super: func
// NESTED_CLOSURE_2-NOT: {|}

class OmitKW1 : ProtocolA {
  override#^OMIT_KEYWORD1?keywords=false^#
}

// OMIT_KEYWORD1-NOT:    Decl[Constructor]
// OMIT_KEYWORD1-DAG:    Decl[InstanceMethod]/Super:         func protoAFunc() {|}; name=protoAFunc(){{$}}
// OMIT_KEYWORD1-DAG:    Decl[InstanceMethod]/Super:         func protoAFuncOptional() {|}; name=protoAFuncOptional(){{$}}
// OMIT_KEYWORD1-DAG:    Decl[Subscript]/Super:              subscript(a: TagPA) -> Int {|}; name=subscript(:)
// OMIT_KEYWORD1-DAG:    Decl[InstanceVar]/Super:            var protoAVarRW: Int{{; name=.+$}}
// OMIT_KEYWORD1-NOT:    Decl[Constructor]

class OmitKW2 : ProtocolA {
  override func#^OMIT_KEYWORD2?keywords=false^#
}

// OMIT_KEYWORD2-NOT:    Decl[InstanceVar]/Super:            var protoAVarRW: Int{{; name=.+$}}
// OMIT_KEYWORD2-NOT:    Decl[Constructor]
// OMIT_KEYWORD2-NOT:    Decl[Subscript]
// OMIT_KEYWORD2-DAG:    Decl[InstanceMethod]/Super:         protoAFunc() {|}; name=protoAFunc(){{$}}
// OMIT_KEYWORD2-DAG:    Decl[InstanceMethod]/Super:         protoAFuncOptional() {|}; name=protoAFuncOptional(){{$}}
// OMIT_KEYWORD2-NOT:    Decl[InstanceVar]/Super:            var protoAVarRW: Int{{; name=.+$}}
// OMIT_KEYWORD2-NOT:    Decl[Constructor]
// OMIT_KEYWORD2-NOT:    Decl[Subscript]

class OmitKW3 : ProtocolA {
  func#^OMIT_KEYWORD3?keywords=false^#
}

// FIXME: missing 'override'
// OMIT_KEYWORD3-NOT:    Decl[InstanceVar]/Super:            var protoAVarRW: Int{{; name=.+$}}
// OMIT_KEYWORD3-NOT:    Decl[Constructor]
// OMIT_KEYWORD2-NOT:    Decl[Subscript]
// OMIT_KEYWORD3-DAG:    Decl[InstanceMethod]/Super:         protoAFunc() {|}; name=protoAFunc(){{$}}
// OMIT_KEYWORD3-DAG:    Decl[InstanceMethod]/Super:         protoAFuncOptional() {|}; name=protoAFuncOptional(){{$}}
// OMIT_KEYWORD3-NOT:    Decl[InstanceVar]/Super:            var protoAVarRW: Int{{; name=.+$}}
// OMIT_KEYWORD3-NOT:    Decl[Constructor]
// OMIT_KEYWORD2-NOT:    Decl[Subscript]

class OmitKW4: ProtocolA {
  var #^OMIT_KEYWORD4?keywords=false^#
}
class OmitKW4_let: ProtocolA {
  let #^OMIT_KEYWORD4_LET?check=OMIT_KEYWORD4;keywords=false^#
}

// OMIT_KEYWORD4-NOT:    Decl[Constructor]
// OMIT_KEYWORD4-NOT:    Decl[InstanceMethod]
// OMIT_KEYWORD4:        Decl[InstanceVar]/Super: protoAVarRW: Int{{; name=.+$}}
// OMIT_KEYWORD4-NOT:    Decl[InstanceMethod]
// OMIT_KEYWORD4-NOT:    Decl[Constructor]
// OMIT_KEYWORD4-NOT:    Decl[Subscript]

class OmitKW5: ProtocolA {
  override
  #^OMIT_KEYWORD5?check=OMIT_KEYWORD1;keywords=false^#
// Same as OMIT_KEYWORD1
}
class OmitKW6: ProtocolA {
  override
  func
  #^OMIT_KEYWORD6?check=OMIT_KEYWORD2;keywords=false^#
// Same as OMIT_KEYWORD2
}
class OmitKW7: ProtocolA {
  func
  #^OMIT_KEYWORD7?check=OMIT_KEYWORD3;keywords=false^#
// Same as OMIT_KEYWORD3
}

class OmitKW8: ProtocolA {
  var
  #^OMIT_KEYWORD8?check=OMIT_KEYWORD4;keywords=false^#
// Same as OMIT_KEYWORD4
}
class OmitKW8_let: ProtocolA {
  let
  #^OMIT_KEYWORD8_LET?check=OMIT_KEYWORD4;keywords=false^#
}
class OmitKW9: ProtocolA {
  override
  var
  #^OMIT_KEYWORD9?check=OMIT_KEYWORD4;keywords=false^#
// Same as OMIT_KEYWORD4
}
class OmitKW9_let: ProtocolA {
  override
  let
  #^OMIT_KEYWORD9_LET?check=OMIT_KEYWORD4;keywords=false^#
// Same as OMIT_KEYWORD4
}
class OmitKW10: ProtocolA {
  override func protoAFunc() {}; #^OMIT_KEYWORD10?check=WITH_PA_NO_PROTOFUNCA;keywords=false^#
// WITH_PA
}

// https://github.com/apple/swift/issues/45165

protocol P_45165 {
  func foo<S : Sequence>(x: S) where S.Iterator.Element == Int
}
class C_45165: P_45165 {
  #^ISSUE_45165_WHERE_CLAUSE?keywords=false^#
}

// ISSUE_45165_WHERE_CLAUSE: Decl[InstanceMethod]/Super: func foo<S>(x: S) where S : Sequence, S.Element == Int {|};

protocol HasThrowingProtocol {
  func foo() throws
}

class HasThrowing {
  func bar() throws {}
  func baz(x: @escaping () throws -> ()) rethrows {}
  init() throws {}
}
class TestClassWithThrows : HasThrowing, HasThrowingProtocol {
  #^HAS_THROWING?keywords=false^#
}
// HAS_THROWING-DAG: Decl[InstanceMethod]/Super:         func foo() throws {|}; name=foo()
// HAS_THROWING-DAG: Decl[InstanceMethod]/Super:         override func bar() throws {|}; name=bar()
// HAS_THROWING-DAG: Decl[InstanceMethod]/Super:         override func baz(x: @escaping () throws -> ()) rethrows {|}; name=baz(x:)
// HAS_THROWING-DAG: Decl[Constructor]/Super:            override init() throws {|}; name=init()

protocol P0
protocol P1 {
  associatedtype T1 = Int
  associatedtype T2 : P0
  associatedtype T3
}
class C1 : P1 {
  #^ASSOC_TYPE1?keywords=false^#
}

// ASSOC_TYPE1: Begin completions, 2 items
// ASSOC_TYPE1: Decl[AssociatedType]/Super:         typealias T2 = {#(Type)#}; name=T2 =
// ASSOC_TYPE1: Decl[AssociatedType]/Super:         typealias T3 = {#(Type)#}; name=T3 =

class Deprecated1 {
  @available(*, deprecated)
  func deprecated() {}
}

class Deprecated2 : Deprecated1 {
  override func #^DEPRECATED_1?keywords=false^#
}
// DEPRECATED_1: Decl[InstanceMethod]/Super/NotRecommended: deprecated() {|};

class EscapingBase {
  func method(_ x: @escaping (@escaping ()->()) -> (()->())) -> ((@escaping ()->() )->()) { }
  func autoclosure(arg: @autoclosure () -> Bool) {}
}
class Escaping : EscapingBase {
  override func #^ESCAPING_1?keywords=false^#
}
// ESCAPING_1: Decl[InstanceMethod]/Super:         method(_ x: @escaping (@escaping () -> ()) -> (() -> ())) -> ((@escaping () -> ()) -> ()) {|};
// ESCAPING_1: Decl[InstanceMethod]/Super:         autoclosure(arg: @autoclosure () -> Bool) {|};
class OverrideBase {
  static let staticLet = 0
  static var staticVar = 0
  static var staticGetOnlyVar: Int { return 0 }

  class let classLet = 0
  class var classVar = 0
  class var classGetOnlyVar: Int { return 0 }

  static func staticMethod() {}
  class func classMethod() {}

  let letDecl = 0
  var varDecl: Int = 0

  init(x: Int) {}
  convenience init(y: Int) { self.init(x: y) }
  required init(a: Int) {}
  required convenience init(b: Int) {}
  func defaultMethod() {}
  final func finalMethod() {}
  open func openMethod() {}
}
protocol OverrideP {
  associatedtype Assoc : OverrideP
  init(p: Int)
}

class Override1 : OverrideBase, OverrideP {
  #^MODIFIER1?keywords=false^#
}
class Override2 : OverrideBase, OverrideP {
  final #^MODIFIER2?keywords=false^#
}
class Override3 : OverrideBase, OverrideP {
  open #^MODIFIER3?check=MODIFIER2;keywords=false^#
  // Same as MODIFIER2.
}
class Override4 : OverrideBase, OverrideP {
  required #^MODIFIER4?keywords=false^#
}
class Override5 : OverrideBase, OverrideP {
  convenience #^MODIFIER5?keywords=false^#
}
class Override6 : OverrideBase, OverrideP {
  typealias #^MODIFIER6?keywords=false^#
}
class Override7 : OverrideBase, OverrideP {
  override #^MODIFIER7?keywords=false^#
}
class Override8 : OverrideBase, OverrideP {
  // Note: This *does* emit functions. It will result invalid decl, but fix-it
  // will do the job.
  convenience func #^MODIFIER8?keywords=false^#
}
class Override9 : OverrideBase, OverrideP {
  // Ditto.
  required var #^MODIFIER9?keywords=false^#
}
class Override10 : OverrideBase, OverrideP {
  // Ditto.
  final typealias #^MODIFIER10?check=MODIFIER6;keywords=false^#
  // Same as MODIFIER6.
}
class Override11 : OverrideBase, OverrideP {
  var #^MODIFIER11?check=MODIFIER9;keywords=false^#
  // Same as MODIFIER9.
}
class Override12 : OverrideBase, OverrideP {
  override var #^MODIFIER12?keywords=false^#
}
class Override13 : OverrideBase, OverrideP {
  // No completions.
  let #^MODIFIER13?keywords=false^#
}
class Override14 : OverrideBase, OverrideP {
  // Note: This *does* emit variables. It will result invalid decl, but a
  // diagnostic will tell the user what to do.
  override let #^MODIFIER14?check=MODIFIER12;keywords=false^#
  // Same as MODIFIER12.
}
class Override15 : OverrideBase, OverrideP {
  required static var #^MODIFIER15?keywords=false^#
}
class Override16 : OverrideBase, OverrideP {
  class var #^MODIFIER16?check=MODIFIER15;keywords=false^#
  // Same as MODIFIER15
}
class Override17 : OverrideBase, OverrideP {
  override class var #^MODIFIER17?keywords=false^#
}
class Override18 : OverrideBase, OverrideP {
  // Note: This *does* emit variables. See MODIFIER14
  override static let #^MODIFIER18?check=MODIFIER17;keywords=false^#
  // Same as MODIFIER17
}
class Override19 : OverrideBase, OverrideP {
  // No completions.
  class let #^MODIFIER19?check=MODIFIER13;keywords=false^#
}
class Override20 : OverrideBase, OverrideP {
  // No completions.
  static let #^MODIFIER20?check=MODIFIER13;keywords=false^#
}
class Override21 : OverrideBase, OverrideP {
  override class func #^MODIFIER21?keywords=false^#
}
class Override22 : OverrideBase, OverrideP {
  class func #^MODIFIER22?keywords=false^#
}
class Override23 : OverrideBase, OverrideP {
  static #^MODIFIER23?keywords=false^#
}
class Override24 : OverrideBase, OverrideP {
  override static #^MODIFIER24?keywords=false^#
}
class Override25 : OverrideBase, OverrideP {
  class #^MODIFIER25?check=MODIFIER23;keywords=false^#
  // Same as MODIFIER23
}
class Override26 : OverrideBase, OverrideP {
  class override #^MODIFIER26?check=MODIFIER24;keywords=false^#
  // Same as MODIFIER24
}

// MODIFIER1: Begin completions, 10 items
// MODIFIER1-DAG: Decl[Constructor]/Super:            required init(p: Int) {|}; name=init(p:)
// MODIFIER1-DAG: Decl[StaticMethod]/Super:           override class func classMethod() {|}; name=classMethod()
// MODIFIER1-DAG: Decl[StaticVar]/Super:              override class var classVar: Int; name=classVar
// MODIFIER1-DAG: Decl[StaticVar]/Super:              override class var classGetOnlyVar: Int; name=classGetOnlyVar
// MODIFIER1-DAG: Decl[InstanceMethod]/Super:         override func defaultMethod() {|}; name=defaultMethod()
// MODIFIER1-DAG: Decl[InstanceMethod]/Super:         override func openMethod() {|}; name=openMethod()
// MODIFIER1-DAG: Decl[InstanceVar]/Super:            override var varDecl: Int; name=varDecl
// MODIFIER1-DAG: Decl[Constructor]/Super:            override init(x: Int) {|}; name=init(x:)
// MODIFIER1-DAG: Decl[Constructor]/Super:            required init(a: Int) {|}; name=init(a:)
// MODIFIER1-DAG: Decl[AssociatedType]/Super:         typealias Assoc = {#(Type)#}; name=Assoc =

// MODIFIER2: Begin completions, 6 items
// MODIFIER2-DAG: Decl[StaticVar]/Super:              override class var classVar: Int; name=classVar
// MODIFIER2-DAG: Decl[StaticVar]/Super:              override class var classGetOnlyVar: Int; name=classGetOnlyVar
// MODIFIER2-DAG: Decl[StaticMethod]/Super:           override class func classMethod() {|}; name=classMethod()
// MODIFIER2-DAG: Decl[InstanceMethod]/Super:         override func defaultMethod() {|}; name=defaultMethod()
// MODIFIER2-DAG: Decl[InstanceMethod]/Super:         override func openMethod() {|}; name=openMethod()
// MODIFIER2-DAG: Decl[InstanceVar]/Super:            override var varDecl: Int; name=varDecl

// MODIFIER4: Begin completions, 3 items
// MODIFIER4-DAG: Decl[Constructor]/Super:            init(p: Int) {|}; name=init(p:)
// MODIFIER4-DAG: Decl[Constructor]/Super:            override init(x: Int) {|}; name=init(x:)
// MODIFIER4-DAG: Decl[Constructor]/Super:            init(a: Int) {|}; name=init(a:)

// MODIFIER5: Begin completions, 3 items
// MODIFIER5-DAG: Decl[Constructor]/Super:            required init(p: Int) {|}; name=init(p:)
// MODIFIER5-DAG: Decl[Constructor]/Super:            override init(x: Int) {|}; name=init(x:)
// MODIFIER5-DAG: Decl[Constructor]/Super:            required init(a: Int) {|}; name=init(a:)

// MODIFIER6: Begin completions, 1 items
// MODIFIER6-DAG: Decl[AssociatedType]/Super:         Assoc = {#(Type)#}; name=Assoc =

// MODIFIER7: Begin completions, 8 items
// MODIFIER7-DAG: Decl[StaticVar]/Super:              class var classVar: Int; name=classVar
// MODIFIER7-DAG: Decl[StaticVar]/Super:              class var classGetOnlyVar: Int; name=classGetOnlyVar
// MODIFIER7-DAG: Decl[StaticMethod]/Super:           class func classMethod() {|}; name=classMethod()
// MODIFIER7-DAG: Decl[InstanceMethod]/Super:         func defaultMethod() {|}; name=defaultMethod()
// MODIFIER7-DAG: Decl[InstanceVar]/Super:            var varDecl: Int; name=varDecl
// MODIFIER7-DAG: Decl[InstanceMethod]/Super:         func openMethod() {|}; name=openMethod()
// MODIFIER7-DAG: Decl[Constructor]/Super:            init(x: Int) {|}; name=init(x:)
// MODIFIER7-DAG: Decl[Constructor]/Super:            required init(a: Int) {|}; name=init(a:)

// MODIFIER8: Begin completions, 2 items
// MODIFIER8-DAG: Decl[InstanceMethod]/Super/Erase[5]: override func defaultMethod() {|}; name=defaultMethod()
// MODIFIER8-DAG: Decl[InstanceMethod]/Super/Erase[5]: override func openMethod() {|}; name=openMethod()

// MODIFIER9: Begin completions, 1 items
// MODIFIER9-DAG: Decl[InstanceVar]/Super/Erase[4]:   override var varDecl: Int; name=varDecl

// MODIFIER12: Begin completions, 1 items
// MODIFIER12-DAG: Decl[InstanceVar]/Super:           varDecl: Int; name=varDecl

// MODIFIER13-NOT: Begin completions

// MODIFIER15: Begin completions, 2 items
// MODIFIER15-DAG: Decl[StaticVar]/Super/Erase[4]:    override var classVar: Int; name=classVar
// MODIFIER15-DAG: Decl[StaticVar]/Super/Erase[4]:    override var classGetOnlyVar: Int; name=classGetOnlyVar

// MODIFIER17: Begin completions, 2 items
// MODIFIER17-DAG: Decl[StaticVar]/Super:             classVar: Int; name=classVar
// MODIFIER17-DAG: Decl[StaticVar]/Super:             classGetOnlyVar: Int; name=classGetOnlyVar

// MODIFIER21: Begin completions, 1 items
// MODIFIER21: Decl[StaticMethod]/Super:              classMethod() {|}; name=classMethod()

// MODIFIER22: Begin completions, 1 items
// MODIFIER22: Decl[StaticMethod]/Super/Erase[5]:     override func classMethod() {|}; name=classMethod()

// MODIFIER23: Begin completions, 3 items
// MODIFIER23-DAG: Decl[StaticMethod]/Super:          override func classMethod() {|}; name=classMethod()
// MODIFIER23-DAG: Decl[StaticVar]/Super:             override var classVar: Int; name=classVar
// MODIFIER23-DAG: Decl[StaticVar]/Super:             override var classGetOnlyVar: Int; name=classGetOnlyVar

// MODIFIER24: Begin completions, 3 items
// MODIFIER24-DAG: Decl[StaticMethod]/Super:          func classMethod() {|}; name=classMethod()
// MODIFIER24-DAG: Decl[StaticVar]/Super:             var classVar: Int; name=classVar
// MODIFIER24-DAG: Decl[StaticVar]/Super:             var classGetOnlyVar: Int; name=classGetOnlyVar

protocol RequiredP {
  init(p: Int)
}
class RequiredClass : RequiredP {
  #^PROTOINIT_NORM?keywords=false^#
}
final class RequiredFinal : RequiredP {
  #^PROTOINIT_FINAL?keywords=false^#
}
struct RequiredS : RequiredP {
  #^PROTOINIT_STRUCT?keywords=false^#
}

// PROTOINIT_NORM: Begin completions, 1 items
// PROTOINIT_NORM-DAG: required init(p: Int) {|}; name=init(p:)

// PROTOINIT_FINAL: Begin completions, 1 items
// PROTOINIT_FINAL-DAG: init(p: Int) {|}; name=init(p:)

// PROTOINIT_STRUCT: Begin completions, 1 items
// PROTOINIT_STRUCT-DAG: init(p: Int) {|}; name=init(p:)

protocol AssocAndMethod {
  associatedtype T = Int
  associatedtype U: P0
  associatedtype V

  func f1(_: T)
  func f2(_: U)
  func f3(_: V)
}

struct MissingAssoc: AssocAndMethod {
  func #^MISSING_ASSOC_1?keywords=false^#
}
// MISSING_ASSOC_1-DAG: Decl[InstanceMethod]/Super:         f1(_: T) {|};
// MISSING_ASSOC_1-DAG: Decl[InstanceMethod]/Super:         f2(_: U) {|};
// MISSING_ASSOC_1-DAG: Decl[InstanceMethod]/Super:         f3(_: V) {|};

// Test that we don't skip out on synthesized conformance members.

struct SynthesizedConformance1: Codable {
  let foo: Int
  #^OVERRIDE_SYNTHESIZED_1?keywords=false^#
// OVERRIDE_SYNTHESIZED_1: Begin completions,  2 items
// OVERRIDE_SYNTHESIZED_1-DAG: Decl[Constructor]/Super/IsSystem:       init(from decoder: any Decoder) throws {|};
// OVERRIDE_SYNTHESIZED_1-DAG: Decl[InstanceMethod]/Super/IsSystem:    func encode(to encoder: any Encoder) throws {|};
}

open class SynthesizedConformance2: Codable {
  let foo: Int
  func encode(to encoder: Encoder) throws {}
  #^OVERRIDE_SYNTHESIZED_2?keywords=false^#
// OVERRIDE_SYNTHESIZED_2: Begin completions, 1 items
// OVERRIDE_SYNTHESIZED_2: Decl[Constructor]/Super/IsSystem:           public required init(from decoder: any Decoder) throws {|};
}

struct SynthesizedConformance3: Hashable {
  let foo: Int
  #^OVERRIDE_SYNTHESIZED_3?keywords=false^#
// FIXME: Where did Equatable.(==) go?
// OVERRIDE_SYNTHESIZED_3: Begin completions, 2 items
// OVERRIDE_SYNTHESIZED_3-DAG: Decl[InstanceVar]/Super/IsSystem:       var hashValue: Int; name=hashValue
// OVERRIDE_SYNTHESIZED_3-DAG: Decl[InstanceMethod]/Super/IsSystem:    func hash(into hasher: inout Hasher) {|}
}

enum SynthesizedConformance4: CaseIterable {
  case a, b, c, d
  #^OVERRIDE_SYNTHESIZED_4?keywords=false^#
// OVERRIDE_SYNTHESIZED_4: Begin completions, 3 items
// OVERRIDE_SYNTHESIZED_4-DAG: Decl[InstanceVar]/Super/IsSystem:       var hashValue: Int
// OVERRIDE_SYNTHESIZED_4-DAG: Decl[InstanceMethod]/Super/IsSystem:    func hash(into hasher: inout Hasher) {|};
// OVERRIDE_SYNTHESIZED_4-DAG: Decl[StaticVar]/Super/IsSystem:         static var allCases: [SynthesizedConformance4];
}

class SynthesizedConformance5: SynthesizedConformance2 {
  #^OVERRIDE_SYNTHESIZED_5?keywords=false^#
// OVERRIDE_SYNTHESIZED_5: Begin completions, 3 items
// OVERRIDE_SYNTHESIZED_5-DAG: Decl[InstanceMethod]/Super:             override func encode(to encoder: any Encoder) throws {|};
// OVERRIDE_SYNTHESIZED_5-DAG: Decl[Constructor]/Super/IsSystem:       required init(from decoder: any Decoder) throws {|};
// OVERRIDE_SYNTHESIZED_5-DAG: Decl[Constructor]/Super:                required init(from decoder: any Decoder) throws {|};
// FIXME: 'required init(from decoder: Decoder)' is suggested twice
}

// https://github.com/apple/swift/issues/57037
protocol P_57037 {
  var value: Int { get }
}
struct S_57037: P_57037 {
  let foo = val, #^MULTI_VAR_DECL_OVERRIDE^#
// MULTI_VAR_DECL_OVERRIDE:     Begin completions, 1 items
// MULTI_VAR_DECL_OVERRIDE-DAG: Decl[InstanceVar]/Super:            value: Int;
}
