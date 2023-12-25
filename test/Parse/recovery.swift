// RUN: %target-typecheck-verify-swift

//===--- Helper types used in this file.

protocol FooProtocol {}

//===--- Tests.

func garbage() -> () {
  var a : Int
  ) this line is invalid, but we will stop at the keyword below... // expected-error{{expected expression}}
  return a + "a" // expected-error{{binary operator '+' cannot be applied to operands of type 'Int' and 'String'}} expected-note {{overloads for '+' exist with these partially matching parameter lists: (Int, Int), (String, String)}}
  // expected-error@-1 {{no '+' candidates produce the expected contextual result type '()'}}
}

func moreGarbage() -> () {
  ) this line is invalid, but we will stop at the declaration... // expected-error{{expected expression}}
  func a() -> Int { return 4 }
  return a() + "a" // expected-error{{binary operator '+' cannot be applied to operands of type 'Int' and 'String'}} expected-note {{overloads for '+' exist with these partially matching parameter lists: (Int, Int), (String, String)}}
  // expected-error@-1 {{no '+' candidates produce the expected contextual result type '()'}}
}


class Container<T> {
  func exists() -> Bool { return true }
}

func useContainer() -> () {
  var a : Container<not a type [skip this greater: >] >, b : Int // expected-error{{expected '>' to complete generic argument list}} expected-note{{to match this opening '<'}}
  b = 5 // no-warning
  a.exists()
}

@xyz class BadAttributes { // expected-error{{unknown attribute 'xyz'}}
  func exists() -> Bool { return true }
}

// expected-note @+2 {{did you mean 'test'?}}
// expected-note @+1 {{'test' declared here}}
func test(a: BadAttributes) -> () {
  _ = a.exists() // no-warning
}

// Here is an extra random close-brace!
} // expected-error{{extraneous '}' at top level}} {{1-3=}}


//===--- Recovery for braced blocks.

func braceStmt1() {
  { braceStmt1(); } // expected-error {{closure expression is unused}} expected-note {{did you mean to use a 'do' statement?}} {{3-3=do }}
}

func braceStmt2() {
  { () in braceStmt2(); } // expected-error {{closure expression is unused}}
}

func braceStmt3() {
  {  // expected-error {{closure expression is unused}} expected-note {{did you mean to use a 'do' statement?}} {{3-3=do }}
    undefinedIdentifier {} // expected-error {{cannot find 'undefinedIdentifier' in scope}}
  }
}

//===--- Recovery for misplaced 'static'.

static func toplevelStaticFunc() {} // expected-error {{static methods may only be declared on a type}} {{1-8=}}

static struct StaticStruct {} // expected-error {{declaration cannot be marked 'static'}} {{1-8=}}
static class StaticClass {} // expected-error {{declaration cannot be marked 'static'}} {{1-8=}}
static protocol StaticProtocol {} // expected-error {{declaration cannot be marked 'static'}} {{1-8=}}
static typealias StaticTypealias = Int // expected-error {{declaration cannot be marked 'static'}} {{1-8=}}

class ClassWithStaticDecls {
  class var a = 42 // expected-error {{class stored properties not supported}}
}

//===--- Recovery for missing controlling expression in statements.

func missingControllingExprInIf() {
  if

  if { // expected-error {{missing condition in 'if' statement}}
  } // expected-error {{expected '{' after 'if' condition}}
  // expected-error@-2 {{cannot convert value of type 'Void' to expected condition type 'Bool'}}

  if // expected-error {{missing condition in 'if' statement}}
  {
  }

  if true {
  } else if { // expected-error {{missing condition in 'if' statement}}
  }

  // It is debatable if we should do recovery here and parse { true } as the
  // body, but the error message should be sensible.
  if { true } { // expected-error {{missing condition in 'if' statement}} expected-error{{consecutive statements on a line must be separated by ';'}} {{14-14=;}} expected-error {{closure expression is unused}} expected-note {{did you mean to use a 'do' statement?}} {{15-15=do }} expected-warning {{boolean literal is unused}}
  }

  if { true }() { // expected-error {{missing condition in 'if' statement}} expected-error 2 {{consecutive statements on a line must be separated by ';'}} expected-error {{closure expression is unused}} expected-note {{did you mean to use a 'do' statement?}} {{17-17=do }} expected-warning {{boolean literal is unused}}
  }

  // <rdar://problem/18940198>
  if { { } } // expected-error{{missing condition in 'if' statement}} expected-error{{closure expression is unused}} expected-note {{did you mean to use a 'do' statement?}} {{8-8=do }}
}

func missingControllingExprInWhile() {
  while // expected-error {{expected expression, var, or let in 'while' condition}}

  while { // expected-error {{missing condition in 'while' statement}}
  }

  while // expected-error {{missing condition in 'while' statement}}
  {
  }

  // It is debatable if we should do recovery here and parse { true } as the
  // body, but the error message should be sensible.
  while { true } { // expected-error {{missing condition in 'while' statement}} expected-error{{consecutive statements on a line must be separated by ';'}} {{17-17=;}} expected-error {{closure expression is unused}} expected-note {{did you mean to use a 'do' statement?}} {{18-18=do }} expected-warning {{boolean literal is unused}}
  }

  while { true }() { // expected-error {{missing condition in 'while' statement}} expected-error 2 {{consecutive statements on a line must be separated by ';'}} expected-error {{closure expression is unused}} expected-note {{did you mean to use a 'do' statement?}} {{20-20=do }} expected-warning {{boolean literal is unused}}
  }

  // <rdar://problem/18940198>
  while { { } } // expected-error{{missing condition in 'while' statement}} expected-error{{closure expression is unused}} expected-note {{did you mean to use a 'do' statement?}} {{11-11=do }}
}

func missingControllingExprInRepeatWhile() {
  repeat {
  } while // expected-error {{missing condition in 'while' statement}}
  { // expected-error{{closure expression is unused}} expected-note {{did you mean to use a 'do' statement?}} {{3-3=do }}
    missingControllingExprInRepeatWhile();
  }

  repeat {
  } while { true }() // expected-error{{missing condition in 'while' statement}} expected-error{{consecutive statements on a line must be separated by ';'}} {{10-10=;}} expected-warning {{result of call to closure returning 'Bool' is unused}}
}

// https://github.com/apple/swift/issues/42787
func missingWhileInRepeat() {
  repeat {
  } // expected-error {{expected 'while' after body of 'repeat' statement}}
}

func acceptsClosure<T>(t: T) -> Bool { return true }

func missingControllingExprInFor() {
  for ; { // expected-error {{C-style for statement has been removed in Swift 3}}
  }

  for ; // expected-error {{C-style for statement has been removed in Swift 3}}
  { 
  }

  for ; true { // expected-error {{C-style for statement has been removed in Swift 3}}
  }

  for var i = 0; true { // expected-error {{C-style for statement has been removed in Swift 3}}
    i += 1
  }
}

func missingControllingExprInForEach() {
  // expected-error @+3 {{expected pattern}}
  // expected-error @+2 {{expected Sequence expression for for-each loop}}
  // expected-error @+1 {{expected '{' to start the body of for-each loop}}
  for

  // expected-error @+2 {{expected pattern}}
  // expected-error @+1 {{expected Sequence expression for for-each loop}}
  for {
  }

  // expected-error @+2 {{expected pattern}}
  // expected-error @+1 {{expected Sequence expression for for-each loop}}
  for
  {
  }

  // expected-error @+2 {{expected 'in' after for-each pattern}}
  // expected-error @+1 {{expected Sequence expression for for-each loop}}
  for i {
  }

  // expected-error @+2 {{expected 'in' after for-each pattern}}
  // expected-error @+1 {{expected Sequence expression for for-each loop}}
  for var i {
  }

  // expected-error @+2 {{expected pattern}}
  // expected-error @+1 {{expected Sequence expression for for-each loop}}
  for in {
  }

  // expected-error @+1 {{expected pattern}}
  for 0..<12 {
  }

  // expected-error @+3 {{expected pattern}}
  // expected-error @+2 {{expected Sequence expression for for-each loop}}
  // expected-error @+1 {{expected '{' to start the body of for-each loop}}
  for for in {
  }

  for i in { // expected-error {{expected Sequence expression for for-each loop}}
  }

// The #if block is used to provide a scope for the for stmt to force it to end
// where necessary to provoke the crash.
#if true  // <rdar://problem/21679557> compiler crashes on "for{{"
  // expected-error @+2 {{expected pattern}}
  // expected-error @+1 {{expected Sequence expression for for-each loop}}
  for{{ // expected-note 2 {{to match this opening '{'}}
#endif  // expected-error {{expected '}' at end of closure}} expected-error {{expected '}' at end of brace statement}}

#if true
  // expected-error @+2 {{expected pattern}}
  // expected-error @+1 {{expected Sequence expression for for-each loop}}
  for{
    var x = 42
  }
#endif
  
  // https://github.com/apple/swift/issues/48502
  struct User { let name: String? }
  let users = [User]()
  for user in users whe { // expected-error {{expected '{' to start the body of for-each loop}}
    if let name = user.name {
      let key = "\(name)"
    }
  }

  for // expected-error {{expected pattern}} expected-error {{Sequence expression for for-each loop}}
  ; // expected-error {{expected '{' to start the body of for-each loop}}
}

func missingControllingExprInSwitch() {
  switch

  switch { // expected-error {{expected expression in 'switch' statement}}
  } // expected-error {{expected '{' after 'switch' subject expression}}

  switch // expected-error {{expected expression in 'switch' statement}} expected-error {{'switch' statement body must have at least one 'case' or 'default' block}}
  {
  }

  switch { // expected-error {{expected expression in 'switch' statement}}
    case _: return
  }

  switch { // expected-error {{expected expression in 'switch' statement}}
    case Int: return // expected-error {{'is' keyword required to pattern match against type name}} {{10-10=is }} 
    case _: return
  }

  switch { 42 } { // expected-error {{expected expression in 'switch' statement}} expected-error{{all statements inside a switch must be covered by a 'case' or 'default'}} expected-error{{consecutive statements on a line must be separated by ';'}} {{16-16=;}} expected-error{{closure expression is unused}} expected-note{{did you mean to use a 'do' statement?}} {{17-17=do }} // expected-error{{'switch' statement body must have at least one 'case' or 'default' block}}
    case _: return // expected-error{{'case' label can only appear inside a 'switch' statement}}
  }

  switch { 42 }() { // expected-error {{expected expression in 'switch' statement}} expected-error {{all statements inside a switch must be covered by a 'case' or 'default'}} expected-error 2 {{consecutive statements on a line must be separated by ';'}} expected-error{{closure expression is unused}} expected-note{{did you mean to use a 'do' statement?}} {{19-19=do }} // expected-error{{'switch' statement body must have at least one 'case' or 'default' block}}
    case _: return // expected-error{{'case' label can only appear inside a 'switch' statement}}
  }
}

//===--- Recovery for missing braces in nominal type decls.

struct NoBracesStruct1() // expected-error {{expected '{' in struct}}
enum NoBracesUnion1() // expected-error {{expected '{' in enum}}
class NoBracesClass1() // expected-error {{expected '{' in class}}
protocol NoBracesProtocol1() // expected-error {{expected '{' in protocol type}}
extension NoBracesStruct1() // expected-error {{expected '{' in extension}}

struct NoBracesStruct2 // expected-error {{expected '{' in struct}}
enum NoBracesUnion2 // expected-error {{expected '{' in enum}}
class NoBracesClass2 // expected-error {{expected '{' in class}}
protocol NoBracesProtocol2 // expected-error {{expected '{' in protocol type}}
extension NoBracesStruct2 // expected-error {{expected '{' in extension}}

//===--- Recovery for multiple identifiers in decls

protocol Multi ident {}
// expected-error @-1 {{found an unexpected second identifier in protocol declaration; is there an accidental break?}}
// expected-note @-2 {{join the identifiers together}} {{10-21=Multiident}}
// expected-note @-3 {{join the identifiers together with camel-case}} {{10-21=MultiIdent}}

class CCC CCC<T> {}
// expected-error @-1 {{found an unexpected second identifier in class declaration; is there an accidental break?}}
// expected-note @-2 {{join the identifiers together}} {{7-14=CCCCCC}}

enum EE EE<T> where T : Multi {
// expected-error @-1 {{found an unexpected second identifier in enum declaration; is there an accidental break?}} 
// expected-note @-2 {{join the identifiers together}} {{6-11=EEEE}}
  
  case a a
  // expected-error @-1 {{found an unexpected second identifier in enum 'case' declaration; is there an accidental break?}} 
  // expected-note @-2 {{join the identifiers together}} {{8-11=aa}}
  // expected-note @-3 {{join the identifiers together with camel-case}} {{8-11=aA}}
  
  case b
}

struct SS SS : Multi {
// expected-error @-1 {{found an unexpected second identifier in struct declaration; is there an accidental break?}}
// expected-note @-2 {{join the identifiers together}} {{8-13=SSSS}}
  
  private var a b : Int = ""
  // expected-error @-1 {{found an unexpected second identifier in variable declaration; is there an accidental break?}}
  // expected-note @-2 {{join the identifiers together}} {{15-18=ab}}
  // expected-note @-3 {{join the identifiers together with camel-case}} {{15-18=aB}}
  // expected-error @-4 {{cannot convert value of type 'String' to specified type 'Int'}}
  
  func f() {
    var c d = 5
    // expected-error @-1 {{found an unexpected second identifier in variable declaration; is there an accidental break?}}
    // expected-note @-2 {{join the identifiers together}} {{9-12=cd}}
    // expected-note @-3 {{join the identifiers together with camel-case}} {{9-12=cD}}
    // expected-warning @-4 {{initialization of variable 'c' was never used; consider replacing with assignment to '_' or removing it}}
    
    let _ = 0
  }
}

let (efg hij, foobar) = (5, 6)
// expected-error @-1 {{found an unexpected second identifier in constant declaration; is there an accidental break?}}
// expected-note @-2 {{join the identifiers together}} {{6-13=efghij}}
// expected-note @-3 {{join the identifiers together with camel-case}} {{6-13=efgHij}}

_ = foobar // OK.


//===--- Recovery for parse errors in types.

struct ErrorTypeInVarDecl1 {
  var v1 : // expected-error {{expected type}} {{11-11= <#type#>}}
}

struct ErrorTypeInVarDecl2 {
  var v1 : Int. // expected-error {{expected member name following '.'}}
  var v2 : Int
}

struct ErrorTypeInVarDecl3 {
  var v1 : Int< // expected-error {{expected type}}
  var v2 : Int
}

struct ErrorTypeInVarDecl4 {
  var v1 : Int<, // expected-error {{expected type}} {{16-16= <#type#>}}
  var v2 : Int
}

struct ErrorTypeInVarDecl5 {
  var v1 : Int<Int // expected-error {{expected '>' to complete generic argument list}} expected-note {{to match this opening '<'}}
  var v2 : Int
}

struct ErrorTypeInVarDecl6 {
  var v1 : Int<Int, // expected-note {{to match this opening '<'}}
               Int // expected-error {{expected '>' to complete generic argument list}}
  var v2 : Int
}


struct ErrorTypeInVarDecl7 {
  var v1 : Int<Int, // expected-error {{expected type}}
  var v2 : Int
}

struct ErrorTypeInVarDecl8 {
  var v1 : protocol<FooProtocol // expected-error {{expected '>' to complete protocol-constrained type}} expected-note {{to match this opening '<'}}
  var v2 : Int
}

struct ErrorTypeInVarDecl9 {
  var v1 : protocol // expected-error {{expected type}}
  var v2 : Int
}

struct ErrorTypeInVarDecl10 {
  var v1 : protocol<FooProtocol // expected-error {{expected '>' to complete protocol-constrained type}} expected-note {{to match this opening '<'}}
  var v2 : Int
}

struct ErrorTypeInVarDecl11 {
  var v1 : protocol<FooProtocol, // expected-error {{expected type}}
  var v2 : Int
}

func ErrorTypeInPattern1(_: protocol<) { } // expected-error {{expected type}}
func ErrorTypeInPattern2(_: protocol<F) { } // expected-error {{expected '>' to complete protocol-constrained type}}
                                            // expected-note@-1 {{to match this opening '<'}}
                                            // expected-error@-2 {{cannot find type 'F' in scope}}

func ErrorTypeInPattern3(_: protocol<F,) { } // expected-error {{expected type}}
                                             // expected-error@-1 {{cannot find type 'F' in scope}}

struct ErrorTypeInVarDecl12 {
  var v1 : FooProtocol & // expected-error{{expected identifier for type name}}
  var v2 : Int
}

struct ErrorTypeInVarDecl13 { // expected-note {{in declaration of 'ErrorTypeInVarDecl13'}}
  var v1 : & FooProtocol // expected-error {{expected type}} expected-error {{consecutive declarations on a line must be separated by ';'}} expected-error{{expected declaration}} 
  var v2 : Int
}

struct ErrorTypeInVarDecl16 {
  var v1 : FooProtocol & // expected-error {{expected identifier for type name}}
  var v2 : Int
}

func ErrorTypeInPattern4(_: FooProtocol & ) { } // expected-error {{expected identifier for type name}}


struct ErrorGenericParameterList1< // expected-error {{expected an identifier to name generic parameter}} expected-error {{expected '{' in struct}}

struct ErrorGenericParameterList2<T // expected-error {{expected '>' to complete generic parameter list}} expected-note {{to match this opening '<'}} expected-error {{expected '{' in struct}}

struct ErrorGenericParameterList3<T, // expected-error {{expected an identifier to name generic parameter}} expected-error {{expected '{' in struct}}

// Note: Don't move braces to a different line here.
struct ErrorGenericParameterList4< // expected-error {{expected an identifier to name generic parameter}}
{
}

// Note: Don't move braces to a different line here.
struct ErrorGenericParameterList5<T // expected-error {{expected '>' to complete generic parameter list}} expected-note {{to match this opening '<'}}
{
}

// Note: Don't move braces to a different line here.
struct ErrorGenericParameterList6<T, // expected-error {{expected an identifier to name generic parameter}}
{
}

struct ErrorTypeInVarDeclFunctionType1 {
  var v1 : () -> // expected-error {{expected type for function result}}
  var v2 : Int
}

struct ErrorTypeInVarDeclArrayType1 {
  var v1 : Int[+] // expected-error {{array types are now written with the brackets around the element type}}
  // expected-error @-1 {{expected expression after unary operator}}
  // expected-error @-2 {{expected expression}}
  var v2 : Int
}

struct ErrorTypeInVarDeclArrayType2 {
  var v1 : Int[+ // expected-error {{unary operator cannot be separated from its operand}}
                 // expected-error@-1 {{expected ']' in array type}}
                 // expected-note@-2 {{to match this opening '['}}
  var v2 : Int // expected-error {{expected expression}}
}

struct ErrorTypeInVarDeclArrayType3 {
  var v1 : Int[ // expected-error {{expected ']' in array type}}
                // expected-note@-1 {{to match this opening '['}}
  ;  // expected-error {{expected expression}}
  var v2 : Int
}

struct ErrorTypeInVarDeclArrayType4 {
  var v1 : Int[1 // expected-error {{expected ']' in array type}} expected-note {{to match this opening '['}}
  var v2 : Int] // expected-error {{unexpected ']' in type; did you mean to write an array type?}} {{12-12=[}}

}

struct ErrorTypeInVarDeclArrayType5 { // expected-note {{in declaration of 'ErrorTypeInVarDeclArrayType5'}}
  let a1: Swift.Int] // expected-error {{unexpected ']' in type; did you mean to write an array type?}} {{11-11=[}}
  let a2: Set<Int]> // expected-error {{expected '>' to complete generic argument list}} // expected-note {{to match this opening '<'}}
  let a3: Set<Int>] // expected-error {{unexpected ']' in type; did you mean to write an array type?}} {{11-11=[}}
  let a4: Int]? // expected-error {{unexpected ']' in type; did you mean to write an array type?}} {{11-11=[}}
  // expected-error @-1 {{consecutive declarations on a line must be separated by ';'}} // expected-error @-1 {{expected declaration}}
  let a5: Int?] // expected-error {{unexpected ']' in type; did you mean to write an array type?}} {{11-11=[}}
  let a6: [Int]] // expected-error {{unexpected ']' in type; did you mean to write an array type?}} {{11-11=[}}
  let a7: [String: Int]] // expected-error {{unexpected ']' in type; did you mean to write an array type?}} {{11-11=[}}
}

struct ErrorTypeInVarDeclDictionaryType {
  let a1: String: // expected-error {{unexpected ':' in type; did you mean to write a dictionary type?}} {{11-11=[}}
  // expected-error @-1 {{expected dictionary value type}}
  let a2: String: Int] // expected-error {{unexpected ':' in type; did you mean to write a dictionary type?}} {{11-11=[}}
  let a3: String: [Int] // expected-error {{unexpected ':' in type; did you mean to write a dictionary type?}} {{11-11=[}} {{24-24=]}}
  let a4: String: Int // expected-error {{unexpected ':' in type; did you mean to write a dictionary type?}} {{11-11=[}} {{22-22=]}}
}

struct ErrorInFunctionSignatureResultArrayType1 {
  func foo() -> Int[ { // expected-error {{expected '{' in body of function declaration}}
                       // expected-note@-1 {{to match this opening '['}}
    return [0]
  }  // expected-error {{expected ']' in array type}}
  func bar() -> Int] { // expected-error {{unexpected ']' in type; did you mean to write an array type?}} {{17-17=[}}
    return [0]
  }
}

struct ErrorInFunctionSignatureResultArrayType2 {
  func foo() -> Int[0 { // expected-error {{expected ']' in array type}} expected-note {{to match this opening '['}}
    return [0]  // expected-error {{cannot convert return expression of type '[Int]' to return type 'Int'}}
  }
}

struct ErrorInFunctionSignatureResultArrayType3 {
  func foo() -> Int[0] { // expected-error {{array types are now written with the brackets around the element type}} {{17-17=[}} {{20-21=}}
    return [0]
  }
}

struct ErrorInFunctionSignatureResultArrayType4 {
  func foo() -> Int[0_1] { // expected-error {{array types are now written with the brackets around the element type}} {{17-17=[}} {{20-21=}}
    return [0]
  }
}


struct ErrorInFunctionSignatureResultArrayType5 {
  func foo() -> Int[0b1] { // expected-error {{array types are now written with the brackets around the element type}} {{17-17=[}} {{20-21=}}
    return [0]
  }
}


struct ErrorInFunctionSignatureResultArrayType11 { // expected-note{{in declaration of 'ErrorInFunctionSignatureResultArrayType11'}}
  func foo() -> Int[(a){a++}] { // expected-error {{consecutive declarations on a line must be separated by ';'}} {{29-29=;}} expected-error {{expected ']' in array type}} expected-note {{to match this opening '['}} expected-error {{cannot find operator '++' in scope; did you mean '+= 1'?}} expected-error {{cannot find 'a' in scope}} expected-error {{expected declaration}}
  }
}

//===--- Recovery for missing initial value in var decls.

struct MissingInitializer1 {
  var v1 : Int = // expected-error {{expected initial value after '='}}
}

//===--- Recovery for expr-postfix.

func exprPostfix1(x : Int) {
  x. // expected-error {{expected member name following '.'}}
}

func exprPostfix2() {
  _ = .42 // expected-error {{'.42' is not a valid floating point literal; it must be written '0.42'}} {{7-7=0}}
}

//===--- Recovery for expr-super.

class ExprSuper {
  init() {
    super. // expected-error {{expected member name following '.'}} 
  }
}

//===--- Recovery for braces inside a nominal decl.

struct BracesInsideNominalDecl1 { // expected-note{{in declaration of 'BracesInsideNominalDecl1'}}
  { // expected-error {{expected declaration}}
    aaa
  }
  typealias A = Int
}
func use_BracesInsideNominalDecl1() {
  // Ensure that the typealias decl is not skipped.
  var _ : BracesInsideNominalDecl1.A // no-error
}

// https://github.com/apple/swift/issues/43383
class С_43383 {
    print("No one else was in the room where it happened") // expected-note {{'print()' previously declared here}}
    // expected-error @-1 {{expected 'func' keyword in instance method declaration}}
    // expected-error @-2 {{expected '{' in body of function declaration}}
    // expected-error @-3 {{expected parameter name followed by ':'}}
}
extension С_43383 {
    print("The room where it happened, the room where it happened")
    // expected-error @-1 {{expected 'func' keyword in instance method declaration}}
    // expected-error @-2 {{invalid redeclaration of 'print()'}}
    // expected-error @-3 {{expected parameter name followed by ':'}}
}


//===--- Recovery for wrong decl introducer keyword.

class WrongDeclIntroducerKeyword1 {
  notAKeyword() {} // expected-error {{expected 'func' keyword in instance method declaration}}
  func foo() {}
  class func bar() {}
}

//===--- Recovery for wrong inheritance clause.

class Base2<T> {
}

class SubModule {
    class Base1 {}
    class Base2<T> {}
}

// expected-error@+1 {{expected ':' to begin inheritance clause}} {{30-31=: }} {{34-35=}}    
class WrongInheritanceClause1(Int) {}           

// expected-error@+1 {{expected ':' to begin inheritance clause}} {{30-31=: }} {{41-42=}}
class WrongInheritanceClause2(Base2<Int>) {}

// expected-error@+1 {{expected ':' to begin inheritance clause}} {{33-34=: }} {{49-50=}}
class WrongInheritanceClause3<T>(SubModule.Base1) where T:AnyObject {}

// expected-error@+1 {{expected ':' to begin inheritance clause}} {{30-31=: }} {{51-52=}}
class WrongInheritanceClause4(SubModule.Base2<Int>) {}

// expected-error@+1 {{expected ':' to begin inheritance clause}} {{33-34=: }} {{54-55=}}
class WrongInheritanceClause5<T>(SubModule.Base2<Int>) where T:AnyObject {}

// expected-error@+1 {{expected ':' to begin inheritance clause}} {{30-31=: }} 
class WrongInheritanceClause6(Int {}

// expected-error@+1 {{expected ':' to begin inheritance clause}} {{33-34=: }} 
class WrongInheritanceClause7<T>(Int where T:AnyObject {}

class Base {}

// <rdar://problem/18502220> [swift-crashes 078] parser crash on invalid cast in sequence expr
Base=1 as Base=1 // expected-error{{cannot convert value of type 'Int' to type 'Base' in coercion}}
// expected-error@-1 {{cannot assign to immutable expression of type 'Base.Type'}}
// expected-error@-2 {{cannot assign to immutable expression of type 'Base'}}
// expected-error@-3 {{cannot assign value of type '()' to type 'Base.Type'}}
// expected-error@-4 {{cannot assign value of type 'Int' to type 'Base'}}

// <rdar://problem/18634543> Parser hangs at swift::Parser::parseType
public enum TestA {
  // expected-error @+1{{expected '{' in body of function declaration}}
  public static func convertFromExtenndition( // expected-error {{expected parameter name followed by ':'}}
    // expected-error@+1{{expected parameter name followed by ':'}}
    s._core.count != 0, "Can't form a Character from an empty String")
}

public enum TestB {
  // expected-error@+1{{expected '{' in body of function declaration}}
  public static func convertFromExtenndition( // expected-error {{expected parameter name followed by ':'}}
    // expected-error@+1 {{expected parameter name followed by ':'}}
    s._core.count ?= 0, "Can't form a Character from an empty String")
}



// <rdar://problem/18634543> Infinite loop and unbounded memory consumption in parser
class bar {}
var baz: bar
// expected-error@+1{{unnamed parameters must be written with the empty name '_'}}
func foo1(bar!=baz) {} // expected-note {{did you mean 'foo1'?}}
// expected-error@+1{{unnamed parameters must be written with the empty name '_'}}
func foo2(bar! = baz) {}// expected-note {{did you mean 'foo2'?}}

// rdar://19605567
// expected-error@+1{{cannot find 'esp' in scope; did you mean 'test'?}}
switch esp {
case let (jeb):
  // expected-error@+2{{expected an identifier to name generic parameter}}
  // expected-error@+1{{expected '{' in class}}
  class Ceac<}> {}
// expected-error@+1{{extraneous '}' at top level}} {{1-2=}}
}


#if true

// rdar://19605164
// expected-error@+2{{cannot find type 'S' in scope}}
struct Foo19605164 {
func a(s: S[{{g) -> Int {} // expected-note {{to match this opening '['}}
}}} // expected-error {{expected ']' in array type}}
// expected-error@-2{{consecutive statements on a line must be separated by ';'}}
// expected-error@-3{{expected expression}}
#endif
  
// rdar://19605567
// expected-error@+6{{expected '(' for initializer parameters}}
// expected-error@+5{{initializers may only be declared within a type}}
// expected-error@+4{{expected an identifier to name generic parameter}}
// expected-error@+3{{consecutive statements on a line must be separated by ';'}}
// expected-error@+2{{expected expression}}
// expected-error@+1{{extraneous '}' at top level}}
func F() { init<( } )} // expected-note 2{{did you mean 'F'?}}

struct InitializerWithName {
  init x() {} // expected-error {{initializers cannot have a name}} {{8-9=}}
}

struct InitializerWithNameAndParam {
  init a(b: Int) {} // expected-error {{initializers cannot have a name}} {{8-9=}}
  init? c(_ d: Int) {} // expected-error {{initializers cannot have a name}} {{9-10=}}
  init e<T>(f: T) {} // expected-error {{initializers cannot have a name}} {{8-9=}}
  init? g<T>(_: T) {} // expected-error {{initializers cannot have a name}} {{9-10=}}
}

struct InitializerWithLabels {
  init c d: Int {}
  // expected-error @-1 {{expected '(' for initializer parameters}}
  // expected-error @-2 {{initializer requires a body}}
}

// rdar://20337695
func f1() {

  // expected-error @+6 {{cannot find 'C' in scope}}
  // expected-note @+5 {{did you mean 'n'?}}
  // expected-error @+4 {{unary operator cannot be separated from its operand}} {{11-12=}}
  // expected-error @+3 {{'==' is not a prefix unary operator}}
  // expected-error @+2 {{consecutive statements on a line must be separated by ';'}} {{8-8=;}}
  // expected-error@+1 {{type annotation missing in pattern}}
  let n == C { get {}  // expected-error {{cannot find 'get' in scope}}
  }
}


// <rdar://problem/20489838> QoI: Nonsensical error and fixit if "let" is missing between 'if let ... where' clauses
func testMultiPatternConditionRecovery(x: Int?) {
  // expected-error@+1 {{expected ',' joining parts of a multi-clause condition}} {{15-21=,}}
  if let y = x where y == 0, let z = x {
    _ = y
    _ = z
  }

  if var y = x, y == 0, var z = x {
    z = y; y = z
  }

  if var y = x, z = x { // expected-error {{expected 'var' in conditional}} {{17-17=var }}
    z = y; y = z
  }


  // <rdar://problem/20883210> QoI: Following a "let" condition with boolean condition spouts nonsensical errors
  guard let x: Int? = 1, x == 1 else {  }
  // expected-warning @-1 {{explicitly specified type 'Int?' adds an additional level of optional to the initializer, making the optional check always succeed}} {{16-20=Int}}
}

// rdar://20866942
func testRefutableLet() {
  var e : Int?
  let x? = e  // expected-error {{consecutive statements on a line must be separated by ';'}} {{8-8=;}}
  // expected-error @-1 {{expected expression}}
  // expected-error @-2 {{type annotation missing in pattern}}
}

// <rdar://problem/19833424> QoI: Bad error message when using Objective-C literals (@"Hello") in Swift files
let myString = @"foo" // expected-error {{string literals in Swift are not preceded by an '@' sign}} {{16-17=}}


// <rdar://problem/16990885> support curly quotes for string literals
// expected-error @+1 {{unicode curly quote found, replace with '"'}} {{35-38="}}
let curlyQuotes1 = “hello world!” // expected-error {{unicode curly quote found, replace with '"'}} {{20-23="}}

// expected-error @+1 {{unicode curly quote found, replace with '"'}} {{20-23="}}
let curlyQuotes2 = “hello world!"


// <rdar://problem/21196171> compiler should recover better from "unicode Specials" characters
let ￼tryx  = 123        // expected-error {{invalid character in source file}}  {{5-8= }}


// <rdar://problem/21369926> Malformed Swift Enums crash playground service
enum Rank: Int {  // expected-error {{'Rank' declares raw type 'Int', but does not conform to RawRepresentable and conformance could not be synthesized}}
  case Ace = 1
  case Two = 2.1  // expected-error {{cannot convert value of type 'Double' to raw type 'Int'}}
}

// rdar://22240342 - Crash in diagRecursivePropertyAccess
class r22240342 {
  lazy var xx: Int = {
    foo {  // expected-error {{cannot find 'foo' in scope}}
      let issueView = 42
      issueView.delegate = 12 // expected-error {{value of type 'Int' has no member 'delegate'}}
      
    }
    return 42
    }()
}


// <rdar://problem/22387625> QoI: Common errors: 'let x= 5' and 'let x =5' could use Fix-its
func r22387625() {
  let _= 5 // expected-error{{'=' must have consistent whitespace on both sides}} {{8-8= }}
  let _ =5 // expected-error{{'=' must have consistent whitespace on both sides}} {{10-10= }}
}
// https://github.com/apple/swift/issues/45723
do {
  let _: Int= 5 // expected-error{{'=' must have consistent whitespace on both sides}} {{13-13= }}
  let _: Array<Int>= [] // expected-error{{'=' must have consistent whitespace on both sides}} {{20-20= }}
}


// <rdar://problem/23086402> Swift compiler crash in CSDiag
protocol A23086402 {
  var b: B23086402 { get }
}

protocol B23086402 {
  var c: [String] { get }
}

func test23086402(a: A23086402) {
  print(a.b.c + "") // should not crash but: expected-error {{}}
}

// <rdar://problem/23550816> QoI: Poor diagnostic in argument list of "print" (varargs related)
// The situation has changed. String now conforms to the RangeReplaceableCollection protocol
// and `ss + s` becomes ambiguous. Disambiguation is provided with the unavailable overload
// in order to produce a meaningful diagnostics. (Related: <rdar://problem/31763930>)
func test23550816(ss: [String], s: String) {
  print(ss + s)  // expected-error {{'+' is unavailable: Operator '+' cannot be used to append a String to a sequence of strings}}
}

// <rdar://problem/23719432> [practicalswift] Compiler crashes on &(Int:_)
func test23719432() {
  var x = 42
    &(Int:x) // expected-error {{'&' may only be used to pass an argument to inout parameter}}
}

// <rdar://problem/19911096> QoI: terrible recovery when using '·' for an operator
infix operator · {  // expected-error {{'·' is considered an identifier and must not appear within an operator name}}
  associativity none precedence 150
}

infix operator -@-class Recover1 {} // expected-error {{'@' is not allowed in operator names}}
prefix operator -фф--class Recover2 {} // expected-error {{'фф' is considered an identifier and must not appear within an operator name}}

// <rdar://problem/21712891> Swift Compiler bug: String subscripts with range should require closing bracket.
func r21712891(s : String) -> String {
  let a = s.startIndex..<s.startIndex
  _ = a
  // The specific errors produced don't actually matter, but we need to reject this.
  return "\(s[a)"  // expected-error {{expected ']' in expression list}} expected-note {{to match this opening '['}}
}


// <rdar://problem/24029542> "Postfix '.' is reserved" error message" isn't helpful
func postfixDot(a : String) {
  _ = a.utf8
  _ = a.   utf8  // expected-error {{extraneous whitespace after '.' is not permitted}} {{9-12=}}
  _ = a.       // expected-error {{expected member name following '.'}}
    a.         // expected-error {{expected member name following '.'}}
}

// <rdar://problem/22290244> QoI: "UIColor." gives two issues, should only give one
func f() { // expected-note 2{{did you mean 'f'?}}
  _ = ClassWithStaticDecls.  // expected-error {{expected member name following '.'}}
}


// rdar://problem/22478168
// https://github.com/apple/swift/issues/53396
// expected-error@+1 {{expected '=' instead of '==' to assign default value for parameter}} {{21-23==}}
func f_53396(a: Int == 0) {}

// rdar://38225184
extension Collection where Element == Int && Index == Int {}
// expected-error@-1 {{expected ',' to separate the requirements of this 'where' clause}} {{43-45=,}}

func testSkipUnbalancedParen() {
  ?( // expected-error {{expected expression}}
}
func testSkipToFindOpenBrace1() {
  // expected-error@+3 {{expected pattern}}
  // expected-error@+2 {{variable binding in a condition requires an initializer}}
  // expected-error@+1 {{expected '{' after 'if' condition}}
  do { if case }
}
func testSkipToFindOpenBrace2() {
  do { if true {} else false } // expected-error {{expected '{' or 'if' after 'else'}}
}

struct Outer {
  struct Inner<T> {}
}
extension Outer.Inner<Never> { // expected-note {{in extension of 'Outer.Inner<Never>'}}
  @someAttr
} // expected-error {{expected declaration}}
