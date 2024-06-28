// RUN: %batch-code-completion -code-completion-annotate-results -code-completion-sourcetext

struct MyStruct {
  init(x: Int) {}
  init<T, U>(_: T.Type, _: U.Type, value: Int = 1) {}
  var propNormal: Int { fatalError() }
  var propFunction: () -> MyStruct { fatalError() }
  func labelNameParamName(label param: (inout Int) throws -> MyStruct) rethrows {}
  func labelName(label: (@autoclosure () -> Int) -> Int) {}
  func sameName(label label: inout Int) {}
  func paramName(_ param: Int) {}
  func emptyName<T>(_: T.Type) {}
  subscript(param: Int) -> Int { 1 }
  subscript(label param: Int) -> Int { 1 }

  static func +(lhs: MyStruct, rhs: MyStruct) -> MyStruct { fatalError() }

  static var instance: MyStruct { fatalError() }
  static func create(x: Int) -> MyStruct { fatalError() }
}

func testGlobal() {
  #^GLOBAL_EXPR^#
}
// GLOBAL_EXPR-DAG: Decl[Struct]/CurrModule:            <name>MyStruct</name>; typename=<typeid.user>MyStruct</typeid.user>;
// GLOBAL_EXPR-DAG: Keyword[class]/None/Flair[RareKeyword]: <keyword>class</keyword>; typename=;
// GLOBAL_EXPR-DAG: Keyword[enum]/None/Flair[RareKeyword]: <keyword>enum</keyword>; typename=;
// GLOBAL_EXPR-DAG: Keyword[if]/None:                   <keyword>if</keyword>; typename=;
// GLOBAL_EXPR-DAG: Keyword[guard]/None:                <keyword>guard</keyword>; typename=;
// GLOBAL_EXPR-DAG: Keyword[try]/None:                  <keyword>try</keyword>; typename=;
// GLOBAL_EXPR-DAG: Keyword[try]/None:                  <keyword>try!</keyword>; typename=;
// GLOBAL_EXPR-DAG: Keyword/None:                       <keyword>Any</keyword>; typename=<keyword>Any</keyword>;
// GLOBAL_EXPR-DAG: Literal[Integer]/None:              0; typename=<typeid.sys>Int</typeid.sys>;
// GLOBAL_EXPR-DAG: Literal[Boolean]/None:              <name>true</name>; typename=<typeid.sys>Bool</typeid.sys>;
// GLOBAL_EXPR-DAG: Literal[Boolean]/None:              <name>false</name>; typename=<typeid.sys>Bool</typeid.sys>;
// GLOBAL_EXPR-DAG: Literal[Nil]/None:                  <name>nil</name>; typename=;
// GLOBAL_EXPR-DAG: Literal[String]/None:               &quot;<callarg><callarg.param>abc</callarg.param></callarg>&quot;; typename=<typeid.sys>String</typeid.sys>;
// GLOBAL_EXPR-DAG: Literal[Array]/None:                [<callarg><callarg.param>values</callarg.param></callarg>]; typename=<typeid.sys>Array</typeid.sys>;
// GLOBAL_EXPR-DAG: Literal[Dictionary]/None:           [<callarg><callarg.param>key</callarg.param></callarg>: <callarg><callarg.param>value</callarg.param></callarg>]; typename=<typeid.sys>Dictionary</typeid.sys>;
// GLOBAL_EXPR-DAG: Literal[_Color]/None:               <name>#colorLiteral</name>(<callarg><callarg.label>red</callarg.label>: <callarg.type><typeid.sys>Float</typeid.sys></callarg.type></callarg>, <callarg><callarg.label>green</callarg.label>: <callarg.type><typeid.sys>Float</typeid.sys></callarg.type></callarg>, <callarg><callarg.label>blue</callarg.label>: <callarg.type><typeid.sys>Float</typeid.sys></callarg.type></callarg>, <callarg><callarg.label>alpha</callarg.label>: <callarg.type><typeid.sys>Float</typeid.sys></callarg.type></callarg>); typename=;
// GLOBAL_EXPR-DAG: Literal[_Image]/None:               <name>#imageLiteral</name>(<callarg><callarg.label>resourceName</callarg.label>: <callarg.type><typeid.sys>String</typeid.sys></callarg.type></callarg>); typename=;
// GLOBAL_EXPR-DAG: Literal[Tuple]/None:                (<callarg><callarg.param>values</callarg.param></callarg>); typename=;
// GLOBAL_EXPR-DAG: Decl[Module]/None/IsSystem:         <name>Swift</name>; typename=Module;
// GLOBAL_EXPR-DAG: Decl[Struct]/OtherModule[Swift]/IsSystem: <name>Int</name>; typename=<typeid.sys>Int</typeid.sys>;
// GLOBAL_EXPR-DAG: Decl[FreeFunction]/OtherModule[Swift]/IsSystem: <name>print</name>(<callarg><callarg.label>_</callarg.label> <callarg.param>items</callarg.param>: <callarg.type><keyword>Any</keyword></callarg.type>...</callarg>, <callarg><callarg.label>to</callarg.label> <callarg.param>output</callarg.param>: &amp;<callarg.type><typeid.sys>TextOutputStream</typeid.sys></callarg.type></callarg>); typename=<typeid.sys>Void</typeid.sys>;


func testType(value: #^GLOBAL_TYPE^#) {}
// GLOBAL_TYPE-DAG: Keyword/None:                       <keyword>Any</keyword>; typename=<keyword>Any</keyword>;
// GLOBAL_TYPE-DAG: Decl[Struct]/CurrModule:            <name>MyStruct</name>; typename=<typeid.user>MyStruct</typeid.user>;
// GLOBAL_TYPE-DAG: Decl[Module]/None:                  <name>swift_ide_test</name>; typename=Module;
// GLOBAL_TYPE-DAG: Decl[Module]/None/IsSystem:         <name>Swift</name>; typename=Module;
// GLOBAL_TYPE-DAG: Decl[Struct]/OtherModule[Swift]/IsSystem: <name>Int</name>; typename=<typeid.sys>Int</typeid.sys>;


func testMember(value: MyStruct) {
  value.#^EXPR_MEMBER^#
}
// EXPR_MEMBER: Begin completions, 8 items
// EXPR_MEMBER-DAG: Keyword[self]/CurrNominal:          <keyword>self</keyword>; typename=<typeid.user>MyStruct</typeid.user>;
// EXPR_MEMBER-DAG: Decl[InstanceVar]/CurrNominal:      <name>propNormal</name>; typename=<typeid.sys>Int</typeid.sys>;
// EXPR_MEMBER-DAG: Decl[InstanceVar]/CurrNominal:      <name>propFunction</name>; typename=() -&gt; <typeid.user>MyStruct</typeid.user>;
// EXPR_MEMBER-DAG: Decl[InstanceMethod]/CurrNominal: <name>labelNameParamName</name>(<callarg><callarg.label>label</callarg.label> <callarg.param>param</callarg.param>: <callarg.type>(<keyword>inout</keyword> <typeid.sys>Int</typeid.sys>) <keyword>throws</keyword> -&gt; <typeid.user>MyStruct</typeid.user></callarg.type></callarg>) <keyword>rethrows</keyword>; typename=<typeid.sys>Void</typeid.sys>;
// EXPR_MEMBER-DAG: Decl[InstanceMethod]/CurrNominal: <name>labelName</name>(<callarg><callarg.label>label</callarg.label>: <callarg.type>(<attribute>@autoclosure</attribute> () -&gt; <typeid.sys>Int</typeid.sys>) -&gt; <typeid.sys>Int</typeid.sys></callarg.type></callarg>); typename=<typeid.sys>Void</typeid.sys>;
// EXPR_MEMBER-DAG: Decl[InstanceMethod]/CurrNominal: <name>sameName</name>(<callarg><callarg.label>label</callarg.label>: &amp;<callarg.type><typeid.sys>Int</typeid.sys></callarg.type></callarg>); typename=<typeid.sys>Void</typeid.sys>;
// EXPR_MEMBER-DAG: Decl[InstanceMethod]/CurrNominal: <name>paramName</name>(<callarg><callarg.label>_</callarg.label> <callarg.param>param</callarg.param>: <callarg.type><typeid.sys>Int</typeid.sys></callarg.type></callarg>); typename=<typeid.sys>Void</typeid.sys>;
// EXPR_MEMBER-DAG: Decl[InstanceMethod]/CurrNominal:   <name>emptyName</name>(<callarg><callarg.label>_</callarg.label>: <callarg.type><typeid.user>T</typeid.user>.Type</callarg.type></callarg>); typename=<typeid.sys>Void</typeid.sys>; name=emptyName(:); sourcetext=emptyName(<#T##T.Type#>)

func testPostfix(value: MyStruct) {
  value #^EXPR_POSTFIX^#
}
// EXPR_POSTFIX: Begin completions, 11 items
// EXPR_POSTFIX-DAG: Decl[InstanceVar]/CurrNominal:      <name>propNormal</name>; typename=<typeid.sys>Int</typeid.sys>;
// EXPR_POSTFIX-DAG: Decl[InstanceVar]/CurrNominal:      <name>propFunction</name>; typename=() -&gt; <typeid.user>MyStruct</typeid.user>;
// EXPR_POSTFIX-DAG: Decl[InstanceMethod]/CurrNominal:   <name>labelNameParamName</name>(<callarg><callarg.label>label</callarg.label> <callarg.param>param</callarg.param>: <callarg.type>(<keyword>inout</keyword> <typeid.sys>Int</typeid.sys>) <keyword>throws</keyword> -&gt; <typeid.user>MyStruct</typeid.user></callarg.type></callarg>) <keyword>rethrows</keyword>; typename=<typeid.sys>Void</typeid.sys>;
// EXPR_POSTFIX-DAG: Decl[InstanceMethod]/CurrNominal:   <name>labelName</name>(<callarg><callarg.label>label</callarg.label>: <callarg.type>(<attribute>@autoclosure</attribute> () -&gt; <typeid.sys>Int</typeid.sys>) -&gt; <typeid.sys>Int</typeid.sys></callarg.type></callarg>); typename=<typeid.sys>Void</typeid.sys>;
// EXPR_POSTFIX-DAG: Decl[InstanceMethod]/CurrNominal:   <name>sameName</name>(<callarg><callarg.label>label</callarg.label>: &amp;<callarg.type><typeid.sys>Int</typeid.sys></callarg.type></callarg>); typename=<typeid.sys>Void</typeid.sys>;
// EXPR_POSTFIX-DAG: Decl[InstanceMethod]/CurrNominal:   <name>paramName</name>(<callarg><callarg.label>_</callarg.label> <callarg.param>param</callarg.param>: <callarg.type><typeid.sys>Int</typeid.sys></callarg.type></callarg>); typename=<typeid.sys>Void</typeid.sys>;
// EXPR_POSTFIX-DAG: Decl[Subscript]/CurrNominal:        [<callarg><callarg.label>_</callarg.label> <callarg.param>param</callarg.param>: <callarg.type><typeid.sys>Int</typeid.sys></callarg.type></callarg>]; typename=<typeid.sys>Int</typeid.sys>;
// EXPR_POSTFIX-DAG: Decl[Subscript]/CurrNominal:        [<callarg><callarg.label>label</callarg.label> <callarg.param>param</callarg.param>: <callarg.type><typeid.sys>Int</typeid.sys></callarg.type></callarg>]; typename=<typeid.sys>Int</typeid.sys>;
// EXPR_POSTFIX-DAG: Keyword[self]/CurrNominal:          <keyword>self</keyword>; typename=<typeid.user>MyStruct</typeid.user>;
// EXPR_POSTFIX-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: <name>+</name>; typename=<typeid.user>MyStruct</typeid.user>;
// EXPR_POSTFIX-DAG: Decl[InstanceMethod]/CurrNominal:   <name>emptyName</name>(<callarg><callarg.label>_</callarg.label>: <callarg.type><typeid.user>T</typeid.user>.Type</callarg.type></callarg>); typename=<typeid.sys>Void</typeid.sys>; name=emptyName(:); sourcetext=.emptyName(<#T##T.Type#>)

func testImplicitMember() -> MyStruct {
  return .#^EXPR_IMPLICITMEMBER^#
}
// EXPR_IMPLICITMEMBER: Begin completions, 10 items
// EXPR_IMPLICITMEMBER-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: <name>init</name>(<callarg><callarg.label>x</callarg.label>: <callarg.type><typeid.sys>Int</typeid.sys></callarg.type></callarg>); typename=<typeid.user>MyStruct</typeid.user>;
// EXPR_IMPLICITMEMBER-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: <name>init</name>(<callarg><callarg.label>_</callarg.label>: <callarg.type><typeid.user>T</typeid.user>.Type</callarg.type></callarg>, <callarg><callarg.label>_</callarg.label>: <callarg.type><typeid.user>U</typeid.user>.Type</callarg.type></callarg>); typename=<typeid.user>MyStruct</typeid.user>; name=init(::); sourcetext=init(<#T##T.Type#>, <#T##U.Type#>)
// EXPR_IMPLICITMEMBER-DAG: Decl[Constructor]/CurrNominal/TypeRelation[Convertible]: <name>init</name>(<callarg><callarg.label>_</callarg.label>: <callarg.type><typeid.user>T</typeid.user>.Type</callarg.type></callarg>, <callarg><callarg.label>_</callarg.label>: <callarg.type><typeid.user>U</typeid.user>.Type</callarg.type></callarg>, <callarg><callarg.label>value</callarg.label>: <callarg.type><typeid.sys>Int</typeid.sys></callarg.type><callarg.default/></callarg>); typename=<typeid.user>MyStruct</typeid.user>; name=init(::value:); sourcetext=init(<#T##T.Type#>, <#T##U.Type#>, value: <#T##Int#>)
// EXPR_IMPLICITMEMBER-DAG: Decl[StaticVar]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: <name>instance</name>; typename=<typeid.user>MyStruct</typeid.user>;
// EXPR_IMPLICITMEMBER-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: <name>labelNameParamName</name>(<callarg><callarg.label>_</callarg.label> <callarg.param>self</callarg.param>: <callarg.type><typeid.user>MyStruct</typeid.user></callarg.type></callarg>); typename=(label: (<keyword>inout</keyword> <typeid.sys>Int</typeid.sys>) <keyword>throws</keyword> -&gt; <typeid.user>MyStruct</typeid.user>) -&gt; <typeid.sys>Void</typeid.sys>;
// EXPR_IMPLICITMEMBER-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: <name>labelName</name>(<callarg><callarg.label>_</callarg.label> <callarg.param>self</callarg.param>: <callarg.type><typeid.user>MyStruct</typeid.user></callarg.type></callarg>); typename=(label: (<attribute>@autoclosure</attribute> () -&gt; <typeid.sys>Int</typeid.sys>) -&gt; <typeid.sys>Int</typeid.sys>) -&gt; <typeid.sys>Void</typeid.sys>;
// EXPR_IMPLICITMEMBER-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: <name>sameName</name>(<callarg><callarg.label>_</callarg.label> <callarg.param>self</callarg.param>: <callarg.type><typeid.user>MyStruct</typeid.user></callarg.type></callarg>); typename=(label: <keyword>inout</keyword> <typeid.sys>Int</typeid.sys>) -&gt; <typeid.sys>Void</typeid.sys>;
// EXPR_IMPLICITMEMBER-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: <name>paramName</name>(<callarg><callarg.label>_</callarg.label> <callarg.param>self</callarg.param>: <callarg.type><typeid.user>MyStruct</typeid.user></callarg.type></callarg>); typename=(<typeid.sys>Int</typeid.sys>) -&gt; <typeid.sys>Void</typeid.sys>;
// EXPR_IMPLICITMEMBER-DAG: Decl[InstanceMethod]/CurrNominal/TypeRelation[Invalid]: <name>emptyName</name>(<callarg><callarg.label>_</callarg.label> <callarg.param>self</callarg.param>: <callarg.type><typeid.user>MyStruct</typeid.user></callarg.type></callarg>); typename=(<typeid.user>T</typeid.user>.Type) -&gt; <typeid.sys>Void</typeid.sys>; name=emptyName(:); sourcetext=emptyName(<#T##self: MyStruct##MyStruct#>)
// EXPR_IMPLICITMEMBER-DAG: Decl[StaticMethod]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: <name>create</name>(<callarg><callarg.label>x</callarg.label>: <callarg.type><typeid.sys>Int</typeid.sys></callarg.type></callarg>); typename=<typeid.user>MyStruct</typeid.user>;

func testArgument() -> MyStruct {
  func foo(x: Int, y: Int) {}
  foo(x: 1, #^CALLARG^#
}
// CALLARG: Begin completions, 1 items
// CALLARG-DAG: Pattern/Local/Flair[ArgLabels]:               <callarg><callarg.label>y</callarg.label>: <callarg.type><typeid.sys>Int</typeid.sys></callarg.type></callarg>; typename=<typeid.sys>Int</typeid.sys>

struct TestArchetypeAnnotations<T> {
  func foo1<U>(u: U, t: T) {}
  func foo2<S: Sequence>(s: S, elt: S.Element) {}
}

func testArchetypeAnnotations<T>(arg: TestArchetypeAnnotations<T>) {
  arg.#^GENERIC^#
}
// GENERIC: Begin completions, 3 items
// GENERIC-DAG: Keyword[self]/CurrNominal:          <keyword>self</keyword>; typename=<typeid.user>TestArchetypeAnnotations</typeid.user>&lt;<typeid.user>T</typeid.user>&gt;; name=self
// GENERIC-DAG: Decl[InstanceMethod]/CurrNominal:   <name>foo1</name>(<callarg><callarg.label>u</callarg.label>: <callarg.type><typeid.user>U</typeid.user></callarg.type></callarg>, <callarg><callarg.label>t</callarg.label>: <callarg.type><typeid.user>T</typeid.user></callarg.type></callarg>); typename=<typeid.sys>Void</typeid.sys>; name=foo1(u:t:)
// GENERIC-DAG: Decl[InstanceMethod]/CurrNominal:   <name>foo2</name>(<callarg><callarg.label>s</callarg.label>: <callarg.type><typeid.sys>Sequence</typeid.sys></callarg.type></callarg>, <callarg><callarg.label>elt</callarg.label>: <callarg.type><typeid.sys>Sequence</typeid.sys>.<typeid.sys>Element</typeid.sys></callarg.type></callarg>); typename=<typeid.sys>Void</typeid.sys>; name=foo2(s:elt:)

struct TestGenericParamAnnotations<T> {
  func foo1<U>(u: U) where #^WHERE^#
}
// WHERE: Begin completions, 4 items
// WHERE-DAG: Decl[GenericTypeParam]/Local:       <name>T</name>; typename=<typeid.user>T</typeid.user>; name=T
// WHERE-DAG: Decl[GenericTypeParam]/Local:       <name>U</name>; typename=<typeid.user>U</typeid.user>; name=U
// WHERE-DAG: Decl[Struct]/Local:                 <name>TestGenericParamAnnotations</name>;
// WHERE-DAG: Keyword[Self]/CurrNominal:          <keyword>Self</keyword>;

protocol BaseP {
  func protoMethod() -> @convention(c) (UInt8) -> Void
  var value: MyStruct
}
class BaseC {
  func baseMethod(x: Int) -> Int { }
  func baseMethodWithName(x y: Int) -> Int { }
  func baseMethodWithEmptyName(_ y: Int) -> Int { }

  func baseMethodAsync(x: Int) async -> Int { }
  func genericAsyncThrowsConstraint<T, U>(x: T) async throws -> U.Element where U: Collection, U.Element == Int {}
  subscript(index: Int) -> (Int) -> Int { }
  subscript(withName index: Int) -> Float { }
  subscript(_ index: String) -> String { }
}
class DerivedC: BaseC, BaseP {
  #^OVERRIDE^#
// OVERRIDE-DAG: Keyword[func]/None:                 <keyword>func</keyword>; typename=; name=func; sourcetext=func
// OVERRIDE-DAG: Decl[InstanceMethod]/Super:         <name>protoMethod</name>() -&gt; (<typeid.sys>UInt8</typeid.sys>) -&gt; <typeid.sys>Void</typeid.sys>; typename=; name=protoMethod(); sourcetext=func protoMethod() -> (UInt8) -> Void {\n<#code#>\n}
// OVERRIDE-DAG: Decl[InstanceVar]/Super:            <name>value</name>: <typeid.user>MyStruct</typeid.user>; typename=; name=value; sourcetext=var value: MyStruct
// OVERRIDE-DAG: Decl[InstanceMethod]/Super:         <name>baseMethod</name>(<param><param.label>x</param.label>: <param.type><typeid.sys>Int</typeid.sys></param.type></param>) -&gt; <typeid.sys>Int</typeid.sys>; typename=; name=baseMethod(x:); sourcetext=override func baseMethod(x: Int) -> Int {\n<#code#>\n}
// OVERRIDE-DAG: Decl[InstanceMethod]/Super:         <name>baseMethodWithName</name>(<param><param.label>x</param.label> <param.param>y</param.param>: <param.type><typeid.sys>Int</typeid.sys></param.type></param>) -&gt; <typeid.sys>Int</typeid.sys>; typename=; name=baseMethodWithName(x:); sourcetext=override func baseMethodWithName(x y: Int) -> Int {\n<#code#>\n}
// OVERRIDE-DAG: Decl[InstanceMethod]/Super:         <name>baseMethodWithEmptyName</name>(<param><param.label>_</param.label> <param.param>y</param.param>: <param.type><typeid.sys>Int</typeid.sys></param.type></param>) -&gt; <typeid.sys>Int</typeid.sys>; typename=; name=baseMethodWithEmptyName(:); sourcetext=override func baseMethodWithEmptyName(_ y: Int) -> Int {\n<#code#>\n}
// OVERRIDE-DAG: Decl[InstanceMethod]/Super:         <name>baseMethodAsync</name>(<param><param.label>x</param.label>: <param.type><typeid.sys>Int</typeid.sys></param.type></param>) <keyword>async</keyword> -&gt; <typeid.sys>Int</typeid.sys>; typename=; name=baseMethodAsync(x:); sourcetext=override func baseMethodAsync(x: Int) async -> Int {\n<#code#>\n}
// OVERRIDE-DAG: Decl[InstanceMethod]/Super:         <name>genericAsyncThrowsConstraint</name>&lt;T, U&gt;(<param><param.label>x</param.label>: <param.type><typeid.user>T</typeid.user></param.type></param>) <keyword>async</keyword> <keyword>throws</keyword> -&gt; <typeid.user>U</typeid.user>.<typeid.sys>Element</typeid.sys> <keyword>where</keyword> <typeid.user>U</typeid.user> : <typeid.sys>Collection</typeid.sys>, <typeid.user>U</typeid.user>.<typeid.sys>Element</typeid.sys> == <typeid.sys>Int</typeid.sys>; typename=; name=genericAsyncThrowsConstraint(x:); sourcetext=override func genericAsyncThrowsConstraint<T, U>(x: T) async throws -> U.Element where U : Collection, U.Element == Int {\n<#code#>\n}
// OVERRIDE-DAG: Decl[Subscript]/Super:              <name>subscript</name>(<param><param.param>index</param.param>: <param.type><typeid.sys>Int</typeid.sys></param.type></param>) -&gt; (<typeid.sys>Int</typeid.sys>) -&gt; <typeid.sys>Int</typeid.sys>; typename=; name=subscript(:); sourcetext=override subscript(index: Int) -> (Int) -> Int {\n<#code#>\n}
// OVERRIDE-DAG: Decl[Subscript]/Super:              <name>subscript</name>(<param><param.label>withName</param.label> <param.param>index</param.param>: <param.type><typeid.sys>Int</typeid.sys></param.type></param>) -&gt; <typeid.sys>Float</typeid.sys>; typename=; name=subscript(withName:); sourcetext=override subscript(withName index: Int) -> Float {\n<#code#>\n}
// OVERRIDE-DAG: Decl[Subscript]/Super:              <name>subscript</name>(<param><param.param>index</param.param>: <param.type><typeid.sys>String</typeid.sys></param.type></param>) -&gt; <typeid.sys>String</typeid.sys>; typename=; name=subscript(:); sourcetext=override subscript(index: String) -> String {\n<#code#>\n}
// OVERRIDE-DAG: Decl[Constructor]/Super:            <name>init</name>(); typename=; name=init(); sourcetext=override init() {\n<#code#>\n}
}

struct Defaults {
  func noDefault(a: Int) { }
  func singleDefault(a: Int = 0) { }
  func mixedDefaults(a: Int, b: Int = 0, c: Int) { }
  func closureDefault(a: (Int) -> Void = {_ in }) { }
}
func defaults(def: Defaults) {
  def.#^DEFAULTS^#
}
// DEFAULTS-DAG: Keyword[self]/CurrNominal:          <keyword>self</keyword>; typename=<typeid.user>Defaults</typeid.user>; name=self; sourcetext=self
// DEFAULTS-DAG: Decl[InstanceMethod]/CurrNominal:   <name>noDefault</name>(<callarg><callarg.label>a</callarg.label>: <callarg.type><typeid.sys>Int</typeid.sys></callarg.type></callarg>); typename=<typeid.sys>Void</typeid.sys>; name=noDefault(a:); sourcetext=noDefault(a: <#T##Int#>)
// DEFAULTS-DAG: Decl[InstanceMethod]/CurrNominal:   <name>singleDefault</name>(); typename=<typeid.sys>Void</typeid.sys>; name=singleDefault(); sourcetext=singleDefault()
// DEFAULTS-DAG: Decl[InstanceMethod]/CurrNominal:   <name>singleDefault</name>(<callarg><callarg.label>a</callarg.label>: <callarg.type><typeid.sys>Int</typeid.sys></callarg.type><callarg.default/></callarg>); typename=<typeid.sys>Void</typeid.sys>; name=singleDefault(a:); sourcetext=singleDefault(a: <#T##Int#>)
// DEFAULTS-DAG: Decl[InstanceMethod]/CurrNominal:   <name>mixedDefaults</name>(<callarg><callarg.label>a</callarg.label>: <callarg.type><typeid.sys>Int</typeid.sys></callarg.type></callarg>, <callarg><callarg.label>c</callarg.label>: <callarg.type><typeid.sys>Int</typeid.sys></callarg.type></callarg>); typename=<typeid.sys>Void</typeid.sys>; name=mixedDefaults(a:c:); sourcetext=mixedDefaults(a: <#T##Int#>, c: <#T##Int#>)
// DEFAULTS-DAG: Decl[InstanceMethod]/CurrNominal:   <name>mixedDefaults</name>(<callarg><callarg.label>a</callarg.label>: <callarg.type><typeid.sys>Int</typeid.sys></callarg.type></callarg>, <callarg><callarg.label>b</callarg.label>: <callarg.type><typeid.sys>Int</typeid.sys></callarg.type><callarg.default/></callarg>, <callarg><callarg.label>c</callarg.label>: <callarg.type><typeid.sys>Int</typeid.sys></callarg.type></callarg>); typename=<typeid.sys>Void</typeid.sys>; name=mixedDefaults(a:b:c:); sourcetext=mixedDefaults(a: <#T##Int#>, b: <#T##Int#>, c: <#T##Int#>)
// DEFAULTS-DAG: Decl[InstanceMethod]/CurrNominal:   <name>closureDefault</name>(); typename=<typeid.sys>Void</typeid.sys>; name=closureDefault(); sourcetext=closureDefault()
// DEFAULTS-DAG: Decl[InstanceMethod]/CurrNominal:   <name>closureDefault</name>(<callarg><callarg.label>a</callarg.label>: <callarg.type>(<typeid.sys>Int</typeid.sys>) -&gt; <typeid.sys>Void</typeid.sys></callarg.type><callarg.default/></callarg>); typename=<typeid.sys>Void</typeid.sys>; name=closureDefault(a:); sourcetext=closureDefault(a: <#T##(Int) -> Void#>)
