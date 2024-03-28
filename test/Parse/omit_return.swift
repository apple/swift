// RUN: %target-swift-frontend %s -typecheck -verify

// MARK: - Helpers



@discardableResult
func logAndReturn<T : CustomStringConvertible>(_ t: T) -> String {
    let log = "\(t)"
    print(log)
    return log
}

@discardableResult
func failableLogAndReturn<T : CustomStringConvertible>(_ t: T) throws -> String {
    let log = "\(t)"
    print(log)
    return log
}


typealias MyOwnVoid = ()

func failableIdentity<T>(_ t: T) throws -> T { t }

enum MyOwnNever {}

func myOwnFatalError() -> MyOwnNever { fatalError() }

struct MyOwnInt : ExpressibleByIntegerLiteral { init(integerLiteral: Int) {} }
struct MyOwnFloat : ExpressibleByFloatLiteral { init(floatLiteral: Double) {} }
struct MyOwnBoolean : ExpressibleByBooleanLiteral { init(booleanLiteral: Bool) {} }
struct MyOwnString : ExpressibleByStringLiteral { init(stringLiteral: String) {} }
struct MyOwnInterpolation : ExpressibleByStringInterpolation { init(stringLiteral: String) {} }

enum Unit {
    case only
}

struct Initable {}

struct StructWithProperty {
    var foo: Int
}

@dynamicMemberLookup
struct DynamicStruct {
    subscript(dynamicMember input: String) -> String { return input } 
}

struct MyOwnArray<Element> : ExpressibleByArrayLiteral {
    init(arrayLiteral elements: Element...) {}
}

struct MyOwnDictionary<Key, Value> : ExpressibleByDictionaryLiteral {
    init(dictionaryLiteral elements: (Key, Value)...) {}
}

struct SubscriptableStruct {
    subscript(int: Int) -> Int { int }
}

extension Int {
    var zero: Int { 0 }
}

extension Optional where Wrapped == Int {
    var someZero: Int? { .some(0) }
}

protocol SomeProto {
    func foo() -> String
}

struct SomeProtoConformer : SomeProto {
    func foo() -> String { "howdy" }
}

class Base {}
class Derived : Base {}

extension Int {
    init() { self = 0 }
}


// MARK: - Notable Free Functions



func identity<T>(_ t: T) -> T { t }

internal func _fatalErrorFlags() -> UInt32 {
    return 0
}
internal func _assertionFailure(
  _ prefix: StaticString, _ message: String,
  flags: UInt32
) -> Never {
    fatalError()
} 
internal func _diagnoseUnexpectedEnumCaseValue<SwitchedValue, RawValue>(
  type: SwitchedValue.Type,
  rawValue: RawValue
) -> Never {
  _assertionFailure("Fatal error",
                    "unexpected enum case '\(type)(rawValue: \(rawValue))'",
                    flags: _fatalErrorFlags())
}


// MARK: - Free Functions



func ff_nop() {
}

func ff_missing() -> String {
}

func ff_implicit() -> String {
    "hello"
}

func ff_explicit() -> String {
    return "hello"
}

func ff_explicitClosure() -> () -> Void {
    return { print("howdy") }
}

func ff_implicitClosure() -> () -> Void {
    { print("howdy") }
}

func ff_explicitMultilineClosure() -> () -> String {
    return {
        let one = "big a"
        let two = "little a"
        return "\(one) + \(two)"
    }
}

func ff_implicitMultilineClosure() -> () -> String {
    {
        let one = "big a"
        let two = "little a"
        return "\(one) + \(two)"
    }
}

func ff_implicitWrong() -> String {
    17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
}

func ff_explicitWrong() -> String {
    return 17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
}

func ff_implicitMulti() -> String {
    print("uh oh")
    "shucks howdy" // expected-warning {{string literal is unused}}
}

func ff_explicitMulti() -> String {
    print("okay")
    return "all right"
}

func ff_effectfulUsed() -> String {
    logAndReturn("okay")
}

// Unused Returns

func ff_effectfulIgnored() {
    logAndReturn("okay")
}

func ff_effectfulIgnoredExplicitReturnTypeVoid() -> Void {
    logAndReturn("okay")
}

func ff_effectfulIgnoredExplicitReturnTypeSwiftVoid() -> Swift.Void {
    logAndReturn("okay")
}

func ff_effectfulIgnoredExplicitReturnTypeMyVoidTypealias() -> MyOwnVoid {
    logAndReturn("okay")
}

func ff_effectfulIgnoredExplicitReturnTypeEmptyTuple() -> () {
    logAndReturn("okay")
}

// Stubs

func ff_stubImplicitReturn() {
    fatalError()
}

func ff_stubExplicitReturnTypeVoid() -> Void {
    fatalError()
}

func ff_stubExplicitReturnTypeSwiftVoid() -> Swift.Void {
    fatalError()
}

func ff_stubExplicitReturnTypeMyVoidTypealias() -> MyOwnVoid {
    fatalError()
}

func ff_stubExplicitReturnTypeEmptyTuple() -> () {
    fatalError()
}

func ff_stubImplicitReturnNever() -> Never {
    fatalError()
}

func ff_stubExplicitReturnNever() -> Never {
    return fatalError()
}

func ff_stubExplicitReturnNeverAsMyOwnNever() -> MyOwnNever {
    return fatalError() // expected-error {{cannot convert return expression of type 'Never' to return type 'MyOwnNever'}}
}

func ff_stubExplicitReturnMyOwnNeverAsNever() -> Never {
    return myOwnFatalError() // expected-error {{cannot convert return expression of type 'MyOwnNever' to return type 'Never'}}
}

func ff_stubImplicitReturnNeverAsMyOwnNever() -> MyOwnNever {
    fatalError()
}

func ff_stubImplicitReturnMyOwnNeverAsNever() -> Never {
    myOwnFatalError()
}

func ff_stubReturnString() -> String {
    fatalError()
}

func ff_stubReturnGeneric<T>() -> T {
    fatalError()
}

// Trying

func ff_tryExplicit() throws -> String {
    return try failableIdentity("shucks")
}

func ff_tryImplicit() throws -> String {
    try failableIdentity("howdy")
}

func ff_tryExplicitMissingThrows() -> String {
    return try failableIdentity("shucks") // expected-error {{errors thrown from here are not handled}}
}

func ff_tryImplicitMissingThrows() -> String {
    try failableIdentity("howdy") // expected-error {{errors thrown from here are not handled}}
}

// Forced Trying

func ff_forceTryExplicit() -> String {
    return try! failableIdentity("howdy")
}

func ff_forceTryImplicit() -> String {
    try! failableIdentity("shucks")
}

func ff_forceTryExplicitAddingThrows() throws -> String {
    return try! failableIdentity("howdy")
}

func ff_forceTryImplicitAddingThrows() throws -> String {
    try! failableIdentity("shucks")
}

// Optional Trying

func ff_optionalTryExplicit() -> String? {
    return try? failableIdentity("howdy")
}

func ff_optionalTryImplicit() -> String? {
    try? failableIdentity("shucks")
}

func ff_optionalTryExplicitAddingThrows() throws -> String? {
    return try? failableIdentity("shucks")
}

func ff_optionalTryImplicitAddingThrows() throws -> String? {
    try? failableIdentity("howdy")
}

// Inferred Return Types

func ff_inferredIntegerLiteralInt() -> Int {
    0
}

func ff_inferredIntegerLiteralInt8() -> Int8 {
    0
}

func ff_inferredIntegerLiteralInt16() -> Int16 {
    0
}

func ff_inferredIntegerLiteralInt32() -> Int32 {
    0
}

func ff_inferredIntegerLiteralInt64() -> Int64 {
    0
}

func ff_inferredIntegerLiteralMyOwnInt() -> MyOwnInt {
    0
}

func ff_nilLiteralInt() -> Int? {
    nil
}

func ff_inferredFloatLiteralFloat() -> Float {
    0.0
}

func ff_inferredFloatLiteralDouble() -> Double {
    0.0
}

func ff_inferredFloatLiteralMyOwnDouble() -> MyOwnFloat {
    0.0
}

func ff_inferredBooleanLiteralBool() -> Bool {
    true
}

func ff_inferredBooleanLiteralMyOwnBoolean() -> MyOwnBoolean {
    true
}

func ff_inferredStringLiteralString() -> String {
    "howdy"
}

func ff_inferredStringLiteralMyOwnString() -> MyOwnString {
    "howdy"
}

func ff_inferredInterpolatedStringLiteralString() -> String {
    "\(0) \(1)"
}

func ff_inferredInterpolatedStringLiteralString() -> MyOwnInterpolation {
    "\(0) \(1)"
}

func ff_inferredMagicFile() -> StaticString {
    #file
}

func ff_inferredMagicLine() -> UInt {
    #line // expected-error {{#line directive was renamed to #sourceLocation}}
} // expected-error {{parameterless closing #sourceLocation() directive without prior opening #sourceLocation(file:,line:) directive}}

func ff_inferredMagicColumn() -> UInt {
    #column
}

func ff_inferredMagicFunction() -> StaticString {
    #function
}

func ff_inferredMagicDSOHandle() -> UnsafeRawPointer {
    #dsohandle
}

func ff_implicitDiscardExpr() {
    _ = 3
}

func ff_implicitMetatype() -> String.Type {
    String.self
}

func ff_implicitMemberRef(_ instance: StructWithProperty) -> Int {
    instance.foo
}

func ff_implicitDynamicMember(_ s: DynamicStruct) -> String {
    s.foo
}

func ff_implicitParenExpr() -> Int {
    (3 + 5)
}

func ff_implicitTupleExpr() -> (Int, Int) {
    (3, 5)
}

func ff_implicitArrayExprArray() -> [Int] {
    [1, 3, 5]
}

func ff_implicitArrayExprSet() -> Set<Int> {
    [1, 3, 5]
}

func ff_implicitArrayExprMyOwnArray() -> MyOwnArray<Int> {
    [1, 3, 5]
}

func ff_implicitDictionaryExprDictionary() -> [Int : Int] {
    [1 : 1, 2 : 2]
}

func ff_implicitDictionaryExprMyOwnDictionary() -> MyOwnDictionary<Int, Int> {
    [1 : 1, 2 : 2]
}

func ff_implicitSubscriptExpr(_ s: SubscriptableStruct) -> Int {
    s[13]
}

func ff_implicitKeyPathExprWritableKeyPath() -> WritableKeyPath<Int, Int> {
    \Int.self
}

func ff_implicitKeyPathExprKeyPath() -> WritableKeyPath<Int, Int> {
    \Int.self.self
}

func ff_implicitTupleElementExpr() -> Int {
    (1,field:2).field
}

func ff_implicitBindExpr(_ opt: Int?) -> Int? {
    opt?.zero
}

func ff_implicitOptionalEvaluation(_ opt: Int?) -> Int? {
    (opt?.zero.zero).someZero
}

func ff_implicitForceValue(_ opt: Int?) -> Int {
    opt!
}

func ff_implicitTemporarilyEscapableExpr(_ cl: () -> Void) -> () -> Void {
    withoutActuallyEscaping(cl) { $0 }
}

func ff_implicitOpenExistentialExpr(_ f: SomeProto) -> String {
    f.foo()
}

func ff_implicitInjectIntoOptionalExpr(_ int: Int) -> Int? {
    int
}

func ff_implicitTupleShuffle(_ input: (one: Int, two: Int)) -> (two: Int, one: Int) {
    input // expected-warning {{expression shuffles the elements of this tuple; this behavior is deprecated}}
}

func ff_implicitCollectionUpcast(_ derived: [Derived]) -> [Base] {
    derived
}

func ff_implicitErasureExpr(_ conformer: SomeProtoConformer) -> SomeProto {
    conformer
}

func ff_implicitAnyHashableErasureExpr(_ int: Int) -> AnyHashable {
    int
}

func ff_implicitCallExpr<Input, Output>(input: Input, function: (Input) -> Output) -> Output {
    function(input)
}

func ff_implicitPrefixUnaryOperator(int: Int) -> Int {
    -int
}

func ff_implicitBinaryOperator(lhs: Int, rhs: Int) -> Int {
    lhs - rhs
}

func ff_implicitConstructorCallRefExpr(lhs: Int, rhs: Int) -> Int {
    Int()
}

func ff_implicitIsExpr<T>(t: T) -> Bool {
    t is Int
}

func ff_implicitCoerceExpr<T>() -> T.Type {
    T.self as T.Type
}

func ff_implicitConditionalCheckedCastExprAs<T>(t: T) -> Int? {
    t as? Int
}

func ff_implicitForceCheckedCastExpr<T>(t: T) -> Int {
    t as! Int
}

func ff_conditional(_ condition: Bool) -> Int {
    condition ? 1 : -1
}

var __ff_implicitAssignExpr: Int = 0
func ff_implicitAssignExpr(newValue: Int) -> Void {
    __ff_implicitAssignExpr = newValue
}

func ff_implicitMemberAccessInit() -> Initable {
    .init()
}

func ff_implicitMemberAccessEnumCase() -> Unit {
    .only
}



// MARK: - Free Properties : Implicit Get



var fv_nop: () {
} // expected-error {{missing return in accessor expected to return '()'}}

var fv_missing: String {
} // expected-error {{missing return in accessor expected to return 'String'}}

var fv_implicit: String {
    "hello"
}

var fv_explicit: String {
    return "hello"
}

var fv_explicitClosure: () -> Void {
    return { print("howdy") }
}

var fv_implicitClosure: () -> Void {
    { print("howdy") }
}

var fv_explicitMultilineClosure: () -> String {
    return {
        let one = "big a"
        let two = "little a"
        return "\(one) + \(two)"
    }
}

var fv_implicitMultilineClosure: () -> String {
    {
        let one = "big a"
        let two = "little a"
        return "\(one) + \(two)"
    }
}

var fv_implicitWrong: String {
    17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
}

var fv_explicitWrong: String {
    return 17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
}

var fv_implicitMulti: String {
    print("uh oh")
    "shucks howdy" // expected-warning {{string literal is unused}}
}

var fv_explicitMulti: String {
    print("okay")
    return "all right"
}

var fv_effectfulUsed: String {
    logAndReturn("okay")
}

// Unused returns

var fv_effectfulIgnored: () {
    logAndReturn("okay")
}

var fv_effectfulIgnoredVoid: Void {
    logAndReturn("okay")
}

var fv_effectfulIgnoredSwiftVoid: Swift.Void {
    logAndReturn("okay")
}

// Stubs

var fv_stubEmptyTuple: () {
    fatalError()
}

var fv_stubVoid: Void {
    fatalError()
}

var fv_stubSwiftVoid: Swift.Void {
    fatalError()
}

var fv_stubMyVoidTypealias: MyOwnVoid {
    fatalError()
}

var fv_stubImplicitReturnNever: Never {
    fatalError()
}

var fv_stubExplicitReturnNever: Never {
    return fatalError()
}

var fv_stubExplicitReturnNeverAsMyOwnNever: MyOwnNever {
    return fatalError() // expected-error {{cannot convert return expression of type 'Never' to return type 'MyOwnNever'}}
}

var fv_stubExplicitReturnMyOwnNeverAsNever: Never {
    return myOwnFatalError() // expected-error {{cannot convert return expression of type 'MyOwnNever' to return type 'Never'}}
}

var fv_stubImplicitReturnNeverAsMyOwnNever: MyOwnNever {
    fatalError()
}

var fv_stubImplicitReturnMyOwnNeverAsNever: Never {
    myOwnFatalError()
}

var fv_stubString: String {
    fatalError()
}

// Forced Trying

var fv_forceTryUnusedExplicit: () {
    return try! failableLogAndReturn("oh") //expected-error {{unexpected non-void return value in void function}}
}

var fv_forceTryUnusedImplicit: () {
    try! failableLogAndReturn("uh") 
}

var fv_forceTryExplicit: String {
    return try! failableIdentity("shucks")
}

var fv_forceTryImplicit: String {
    try! failableIdentity("howdy")
}

// Optional Trying

var fv_optionalTryUnusedExplicit: () {
    return try? failableLogAndReturn("uh")  //expected-error {{unexpected non-void return value in void function}}
}

var fv_optionalTryUnusedImplicit: () {
    try? failableLogAndReturn("oh")
}

var fv_optionalTryExplicit: String? {
    return try? failableIdentity("shucks")
}

var fv_optionalTryImplicit: String? {
    try? failableIdentity("howdy")
}



// MARK: - Free Properties : Get



var fvg_nop: () {
    get {
    }
}

var fvg_missing: String {
    get {
    }
}

var fvg_implicit: String {
    get {
        "hello"
    }
}

var fvg_explicit: String {
    get {
        return "hello"
    }
}

var fvg_explicitClosure: () -> Void {
    get {
        return { print("howdy") }
    }
}

var fvg_implicitClosure: () -> Void {
    get {
        { print("howdy") }
    }
}

var fvg_explicitMultilineClosure: () -> String {
    get {
        return {
            let one = "big a"
            let two = "little a"
            return "\(one) + \(two)"
        }
    }
}

var fvg_implicitMultilineClosure: () -> String {
    get {
        {
            let one = "big a"
            let two = "little a"
            return "\(one) + \(two)"
        }
    }
}

var fvg_implicitWrong: String {
    get {
        17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
    }
}

var fvg_explicitWrong: String {
    get {
        return 17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
    }
}

var fvg_implicitMulti: String {
    get {
        print("uh oh")
        "shucks howdy" // expected-warning {{string literal is unused}}
    }
}

var fvg_explicitMulti: String {
    get {
        print("okay")
        return "all right"
    }
}

var fvg_effectfulUsed: String {
    get {
        logAndReturn("okay")
    }
}

// Unused returns

var fvg_effectfulIgnored: () {
    get {
        logAndReturn("okay")
    }
}

var fvg_effectfulIgnoredVoid: Void {
    get {
        logAndReturn("okay")
    }
}

var fvg_effectfulIgnoredSwiftVoid: Swift.Void {
    get {
        logAndReturn("okay")
    }
}

// Stubs

var fvg_stubEmptyTuple: () {
    get {
        fatalError()
    }
}

var fvg_stubVoid: Void {
    get {
        fatalError()
    }
}

var fvg_stubSwiftVoid: Swift.Void {
    get {
        fatalError()
    }
}

var fvg_stubMyVoidTypealias: MyOwnVoid {
    get {
        fatalError()
    }
}

var fvg_stubImplicitReturnNever: Never {
    get {
        fatalError()
    }
}

var fvg_stubExplicitReturnNever: Never {
    get {
        return fatalError()
    }
}

var fvg_stubExplicitReturnNeverAsMyOwnNever: MyOwnNever {
    get {
        return fatalError() // expected-error {{cannot convert return expression of type 'Never' to return type 'MyOwnNever'}}
    }
}

var fvg_stubExplicitReturnMyOwnNeverAsNever: Never {
    get {
        return myOwnFatalError() // expected-error {{cannot convert return expression of type 'MyOwnNever' to return type 'Never'}}
    }
}

var fvg_stubImplicitReturnNeverAsMyOwnNever: MyOwnNever {
    get {
        fatalError()
    }
}

var fvg_stubImplicitReturnMyOwnNeverAsNever: Never {
    get {
        myOwnFatalError()
    }
}

var fvg_stubString: String {
    get {
        fatalError()
    }
}

// Forced Trying

var fvg_forceTryExplicit: String {
    get {
        return try! failableIdentity("shucks")
    }
}

var fvg_forceTryImplicit: String {
    get {
        try! failableIdentity("howdy")
    }
}

// Optional Trying

var fvg_optionalTryExplicit: String? {
    get {
        return try? failableIdentity("shucks")
    }
}

var fvg_optionalTryImplicit: String? {
    get {
        try? failableIdentity("howdy")
    }
}



// MARK: - Free Properties : Set



var fvs_nop: () {
    get {}
    set {}
}

var fvs_implicit: String {
    get { "ok" }
    set {
        "hello" // expected-warning {{string literal is unused}} 
    }
}

var fvs_explicit: String {
    get { "ok" }
    set {
        return "hello" // expected-error {{unexpected non-void return value in void function}}
    }
}

var fvs_explicitClosure: () -> Void {
    get { return { print("howdy") } }
    set {
        return { print("howdy") } // expected-error {{unexpected non-void return value in void function}}
    }
}

var fvs_implicitClosure: () -> Void {
    get { { print("howdy") } }
    set {
        { print("howdy") } // expected-error {{closure expression is unused}} expected-note {{did you mean to use a 'do' statement?}}
    }
}

var fvs_implicitWrong: String {
    get { "ok" }
    set {
        17 // expected-warning {{integer literal is unused}}
    }
}

var fvs_explicitWrong: String {
    get { "ok" }
    set {
        return 17 // expected-error {{unexpected non-void return value in void function}}
    }
}

var fvs_implicitMulti: String {
    get { "ok" }
    set {
        print("uh oh")
        "shucks howdy" // expected-warning {{string literal is unused}}
    }
}

var fvs_explicitMulti: String {
    get { "ok" }
    set {
        print("okay")
        return "all right" // expected-error {{unexpected non-void return value in void function}}
    }
}

var fvs_effectfulUsed: String {
    get { "ok" }
    set {
        logAndReturn("okay")
    }
}

// Stubs

var fvs_stub: () {
    get { () }
    set {
        fatalError()
    }
}

var fvs_stubMyOwnFatalError: () {
    get { () }
    set {
        myOwnFatalError()
    }
}

// Forced Trying

var fvs_forceTryExplicit: String {
    get { "ok" }
    set {
        return try! failableIdentity("shucks") // expected-error {{unexpected non-void return value in void function}}
    }
}

var fvs_forceTryImplicit: String {
    get { "ok" }
    set {
        try! failableIdentity("howdy") // expected-warning {{result of call to 'failableIdentity' is unused}}
    }
}

// Optional Trying

var fvs_optionalTryExplicit: String? {
    get { "ok" }
    set {
        return try? failableIdentity("shucks") // expected-error {{unexpected non-void return value in void function}}
    }
}

var fvs_optionalTryImplicit: String? {
    get { "ok" }
    set {
        try? failableIdentity("howdy") // expected-warning {{result of 'try?' is unused}}
    }
}



// MARK: - Free Properties : Read






// MARK: - Free Properties : Modify






// MARK: - Subscripts : Implicit Readonly



enum S_nop {
    subscript() -> () {
    } // expected-error {{missing return in subscript expected to return '()'}}
}

enum S_missing {
    subscript() -> String {
    } // expected-error {{missing return in subscript expected to return 'String'}}
}

enum S_implicit {
    subscript() -> String {
        "hello"
    }
}

enum S_explicit {
    subscript() -> String {
        return "hello"
    }
}

enum S_explicitClosure {
    subscript() -> () -> Void {
        return { print("howdy") } 
    }
}

enum S_implicitClosure {
    subscript() -> () -> Void {
        { print("howdy") } 
    }
}

enum S_explicitMultilineClosure {
   subscript() -> () -> String {
        return {
            let one = "big a"
            let two = "little a"
            return "\(one) + \(two)"
        }
    }
}

enum S_implicitMultilineClosure {
    subscript() -> () -> String {
        {
            let one = "big a"
            let two = "little a"
            return "\(one) + \(two)"
        }
    }
}

enum S_implicitWrong {
    subscript() -> String {
        17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
    }
}

enum S_explicitWrong {
    subscript() -> String {
        return 17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
    }
}

enum S_implicitMulti {
    subscript() -> String {
        print("uh oh")
        "shucks howdy" // expected-warning {{string literal is unused}}
    }
}

enum S_explicitMulti {
    subscript() -> String {
        print("okay")
        return "all right"
    }
}

enum S_effectfulUsed {
    subscript() -> String {
        logAndReturn("okay")
    }
}

// Unused returns

enum S_effectfulIgnored {
    subscript() -> () {
        logAndReturn("okay")
    }
}

enum S_effectfulIgnoredVoid {
    subscript() -> Void {
        logAndReturn("okay")
    }
}

enum S_effectfulIgnoredSwiftVoid {
    subscript() -> Swift.Void {
        logAndReturn("okay")
    }
}

// Stubs

enum S_stubEmptyTuple {
    subscript() -> () {
        fatalError()
    }
}

enum S_stubVoid {
    subscript() -> Void {
        fatalError()
    }
}

enum S_stubSwiftVoid {
    subscript() -> Swift.Void {
        fatalError()
    }
}

enum S_stubMyVoidTypealias {
    subscript() -> MyOwnVoid {
        fatalError()
    }
}

enum S_stubImplicitReturnNever {
    subscript() -> Never {
        fatalError()
    }
}

enum S_stubExplicitReturnNever {
    subscript() -> Never {
        return fatalError()
    }
}

enum S_stubExplicitReturnNeverAsMyOwnNever {
    subscript() -> MyOwnNever {
        return fatalError() // expected-error {{cannot convert return expression of type 'Never' to return type 'MyOwnNever'}}
    }
}

enum S_stubExplicitReturnMyOwnNeverAsNever {
    subscript() -> Never {
        return myOwnFatalError() // expected-error {{cannot convert return expression of type 'MyOwnNever' to return type 'Never'}}
    }
}

enum S_stubImplicitReturnNeverAsMyOwnNever {
    subscript() -> MyOwnNever {
        fatalError()
    }
}

enum S_stubImplicitReturnMyOwnNeverAsNever {
    subscript() -> Never {
        myOwnFatalError()
    }
}

enum S_stubString {
    subscript() -> String {
        fatalError()
    }
}

enum S_stubGeneric {
    subscript<T>() -> T {
        fatalError()
    }
}

// Forced Trying

enum S_forceTryExplicit {
    subscript() -> String {
        return try! failableIdentity("shucks")
    }
}

enum S_forceTryImplicit {
    subscript() -> String {
        try! failableIdentity("howdy")
    }
}

// Optional Trying

enum S_optionalTryExplicit {
    subscript() -> String? {
        return try? failableIdentity("shucks")
    }
}

enum S_optionalTryImplicit {
    subscript() -> String? {
        try? failableIdentity("howdy")
    }
}



// MARK: - Subscripts : Explicit Readonly



enum SRO_nop {
    subscript() -> () {
        get {
        }
    }
}

enum SRO_missing {
    subscript() -> String {
        get {
        }
    }
}

enum SRO_implicit {
    subscript() -> String {
        get {
            "hello"
        }
    }
}

enum SRO_explicit {
    subscript() -> String {
        get {
            return "hello"
        }
    }
}

enum SRO_explicitClosure {
    subscript() -> () -> Void {
        get {
            return { print("howdy") } 
        }
    }
}

enum SRO_implicitClosure {
    subscript() -> () -> Void {
        get {
            { print("howdy") } 
        }
    }
}

enum SRO_explicitMultilineClosure {
   subscript() -> () -> String {
        get {
            return {
                let one = "big a"
                let two = "little a"
                return "\(one) + \(two)"
            }
        }
    }
}

enum SRO_implicitMultilineClosure {
    subscript() -> () -> String {
        get {
            {
                let one = "big a"
                let two = "little a"
                return "\(one) + \(two)"
            }
        }
    }
}

enum SRO_implicitWrong {
    subscript() -> String {
        get {
            17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
        }
    }
}

enum SRO_explicitWrong {
    subscript() -> String {
        get {
            return 17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
        }
    }
}

enum SRO_implicitMulti {
    subscript() -> String {
        get {
            print("uh oh")
            "shucks howdy" // expected-warning {{string literal is unused}}
        }
    }
}

enum SRO_explicitMulti {
    subscript() -> String {
        get {
            print("okay")
            return "all right"
        }
    }
}

enum SRO_effectfulUsed {
    subscript() -> String {
        get {
            logAndReturn("okay")
        }
    }
}

// Unused returns

enum SRO_effectfulIgnored {
    subscript() -> () {
        get {
            logAndReturn("okay")
        }
    }
}

enum SRO_effectfulIgnoredVoid {
    subscript() -> Void {
        get {
            logAndReturn("okay")
        }
    }
}

enum SRO_effectfulIgnoredSwiftVoid {
    subscript() -> Swift.Void {
        get {
            logAndReturn("okay")
        }
    }
}

// Stubs

enum SRO_stubEmptyTuple {
    subscript() -> () {
        get {
            fatalError()
        }
    }
}

enum SRO_stubVoid {
    subscript() -> Void {
        get {
            fatalError()
        }
    }
}

enum SRO_stubSwiftVoid {
    subscript() -> Swift.Void {
        get {
            fatalError()
        }
    }
}

enum SRO_stubMyVoidTypealias {
    subscript() -> MyOwnVoid {
        get {
            fatalError()
        }
    }
}

enum SRO_stubImplicitReturnNever {
    subscript() -> Never {
        get {
            fatalError()
        }
    }
}

enum SRO_stubExplicitReturnNever {
    subscript() -> Never {
        get {
            return fatalError()
        }
    }
}

enum SRO_stubExplicitReturnNeverAsMyOwnNever {
    subscript() -> MyOwnNever {
        get {
            return fatalError() // expected-error {{cannot convert return expression of type 'Never' to return type 'MyOwnNever'}}
        }
    }
}

enum SRO_stubExplicitReturnMyOwnNeverAsNever {
    subscript() -> Never {
        get {
            return myOwnFatalError() // expected-error {{cannot convert return expression of type 'MyOwnNever' to return type 'Never'}}
        }
    }
}

enum SRO_stubImplicitReturnNeverAsMyOwnNever {
    subscript() -> MyOwnNever {
        get {
            fatalError()
        }
    }
}

enum SRO_stubImplicitReturnMyOwnNeverAsNever {
    subscript() -> Never {
        get {
            myOwnFatalError()
        }
    }
}

enum SRO_stubString {
    subscript() -> String {
        get {
            fatalError()
        }
    }
}

enum SRO_stubGeneric {
    subscript<T>() -> T {
        get {
            fatalError()
        }
    }
}

// Forced Trying

enum SRO_forceTryExplicit {
    subscript() -> String {
        get {
            return try! failableIdentity("shucks")
        }
    }
}

enum SRO_forceTryImplicit {
    subscript() -> String {
        get {
            try! failableIdentity("howdy")
        }
    }
}

// Optional Trying

enum SRO_optionalTryExplicit {
    subscript() -> String? {
        get {
            return try? failableIdentity("shucks")
        }
    }
}

enum SRO_optionalTryImplicit {
    subscript() -> String? {
        get {
            try? failableIdentity("howdy")
        }
    }
}



// MARK: - Subscripts : Set






// MARK: - Subscripts : Read/Modify






// MARK: - Constructors



struct C_nop {
    init() {
    }
}

struct C_missing {
    var i: Int
    init?() {
    }
}

struct C_implicitNil {
    init?() {
        nil
    }
}

struct C_explicitNil {
    init?() {
        return nil
    }
}

struct C_forcedMissing {
    var i: Int
    init!() {
    }
}

struct C_forcedImplicitNil {
    init!() {
        nil
    }
}

struct C_forcedExplicitNil {
    init?() {
        return nil
    }
}

struct C_implicit {
    init() {
        "hello" // expected-warning {{string literal is unused}}
    }
}

struct C_explicit {
    init() {
        return "hello" // expected-error {{'nil' is the only return value permitted in an initializer}}
    }
}



// MARK: - Destructors



class D_nop {
    deinit {
    }
}

class D_implicit {
    deinit {
        "bye now" // expected-warning {{string literal is unused}}
    }
}

class D_explicit {
    deinit {
        return "bye now" // expected-error {{unexpected non-void return value in void function}}
    }
}

class D_implicitMulti {
    deinit {
        print("okay")
        "see ya" // expected-warning {{string literal is unused}}
    }
}

class D_explicitMulti {
    deinit {
        print("okay")
        return "see ya" // expected-error {{unexpected non-void return value in void function}}
    }
}

// Unused returns

class D_effectfulIgnored {
    deinit {
        logAndReturn("bye now")
    }
}

// Stubs

class D_stub {
    deinit {
        fatalError()
    }
}

class D_stubMyOwnDeinit {
    deinit {
        myOwnFatalError()
    }
}

// Forced Trying

class D_forceTryUnusedExplicit {
    deinit {
        return try! failableLogAndReturn("uh") // expected-error {{unexpected non-void return value in void function}}
    }
}

class D_forceTryUnusedImplicit {
    deinit {
        try! failableLogAndReturn("oh")
    }
}

// Optional Trying

class D_optionalTryUnusedExplicit {
    deinit {
        return try? failableLogAndReturn("uh") // expected-error {{unexpected non-void return value in void function}}
    }
}

class D_optionalTryUnusedImplicit {
    deinit {
        try? failableLogAndReturn("oh")
    }
}



// Miscellaneous

class CSuperExpr_Base { init() {} }
class CSuperExpr_Derived : CSuperExpr_Base { override init() { super.init() } }

class CImplicitIdentityExpr { func gimme() -> CImplicitIdentityExpr { self } }

class CImplicitDotSelfExpr { func gimme() -> CImplicitDotSelfExpr { self.self } }

func badIs<T>(_ value: Any, anInstanceOf type: T.Type) -> Bool { // expected-note {{'type' declared here}}
    value is type // expected-error {{type-casting operator expects a type on its right-hand side (got: parameter 'type')}}
}



// Autoclosure Discriminators



func embedAutoclosure_standard() -> Int {
    _helpEmbedAutoclosure_standard(42)
}
func _helpEmbedAutoclosure_standard<T>(_ value: @autoclosure () -> T) -> T {
    value()
}

func embedAutoclosure_never() -> Int {
  fatalError("unsupported")
}


