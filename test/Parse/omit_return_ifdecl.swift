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

func failableIdentity<T>(_ t: T) throws -> T {
    #if true
    t
    #endif
}

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
    subscript(int: Int) -> Int {
        #if true
        int
        #endif
    }
}

extension Int {
    var zero: Int {
        #if true
        0
        #endif
    }
}

extension Optional where Wrapped == Int {
    var someZero: Int? {
        #if true
        Int?.some(0)
        #endif
    }
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



func identity<T>(_ t: T) -> T {
    #if true
    t
    #endif
}

internal func _fatalErrorFlags() -> UInt32 {
    #if true
    return 0
    #endif
}
internal func _assertionFailure(
  _ prefix: StaticString, _ message: String,
  flags: UInt32
) -> Never {
    #if true
    fatalError()
    #endif
} 
internal func _diagnoseUnexpectedEnumCaseValue<SwitchedValue, RawValue>(
  type: SwitchedValue.Type,
  rawValue: RawValue
) -> Never {
    #if true
  _assertionFailure("Fatal error",
                    "unexpected enum case '\(type)(rawValue: \(rawValue))'",
                    flags: _fatalErrorFlags())
    #endif
}


// MARK: - Free Functions



func ff_nop() {
    #if true
    #endif
}

func ff_nop_false() {
    #if false
    #endif
}

func ff_missing() -> String {
    #if true
    #endif
}

func ff_implicit() -> String {
    #if true
    "hello"
    #endif
}

func ff_explicit() -> String {
    #if true
    return "hello"
    #endif
}

func ff_explicitClosure() -> () -> Void {
    #if true
    return { print("howdy") }
    #endif
}

func ff_implicitClosure() -> () -> Void {
    #if true
    { print("howdy") }
    #endif
}

func ff_explicitMultilineClosure() -> () -> String {
    #if true
    return {
        let one = "big a"
        let two = "little a"
        return "\(one) + \(two)"
    }
    #endif
}

func ff_implicitMultilineClosure() -> () -> String {
    #if true
    {
        let one = "big a"
        let two = "little a"
        return "\(one) + \(two)"
    }
    #endif
}

func ff_implicitWrong() -> String {
    #if true
    17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
    #endif
}

func ff_explicitWrong() -> String {
    #if true
    return 17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
    #endif
}

func ff_implicitMulti() -> String {
    #if true
    print("uh oh")
    "shucks howdy" // expected-warning {{string literal is unused}}
    #endif
}

func ff_explicitMulti() -> String {
    #if true
    print("okay")
    return "all right"
    #endif
}

func ff_effectfulUsed() -> String {
    #if true
    logAndReturn("okay")
    #endif
}

// Unused Returns

func ff_effectfulIgnored() {
    #if true
    logAndReturn("okay")
    #endif
}

func ff_effectfulIgnoredExplicitReturnTypeVoid() -> Void {
    #if true
    logAndReturn("okay")
    #endif
}

func ff_effectfulIgnoredExplicitReturnTypeSwiftVoid() -> Swift.Void {
    #if true
    logAndReturn("okay")
    #endif
}

func ff_effectfulIgnoredExplicitReturnTypeMyVoidTypealias() -> MyOwnVoid {
    #if true
    logAndReturn("okay")
    #endif
}

func ff_effectfulIgnoredExplicitReturnTypeEmptyTuple() -> () {
    #if true
    logAndReturn("okay")
    #endif
}

// Stubs

func ff_stubImplicitReturn() {
    #if true
    fatalError()
    #endif
}

func ff_stubExplicitReturnTypeVoid() -> Void {
    #if true
    fatalError()
    #endif
}

func ff_stubExplicitReturnTypeSwiftVoid() -> Swift.Void {
    #if true
    fatalError()
    #endif
}

func ff_stubExplicitReturnTypeMyVoidTypealias() -> MyOwnVoid {
    #if true
    fatalError()
    #endif
}

func ff_stubExplicitReturnTypeEmptyTuple() -> () {
    #if true
    fatalError()
    #endif
}

func ff_stubImplicitReturnNever() -> Never {
    #if true
    fatalError()
    #endif
}

func ff_stubExplicitReturnNever() -> Never {
    #if true
    return fatalError()
    #endif
}

func ff_stubExplicitReturnNeverAsMyOwnNever() -> MyOwnNever {
    #if true
    return fatalError() // expected-error {{cannot convert return expression of type 'Never' to return type 'MyOwnNever'}}
    #endif
}

func ff_stubExplicitReturnMyOwnNeverAsNever() -> Never {
    #if true
    return myOwnFatalError() // expected-error {{cannot convert return expression of type 'MyOwnNever' to return type 'Never'}}
    #endif
}

func ff_stubImplicitReturnNeverAsMyOwnNever() -> MyOwnNever {
    #if true
    fatalError()
    #endif
}

func ff_stubImplicitReturnMyOwnNeverAsNever() -> Never {
    #if true
    myOwnFatalError()
    #endif
}

func ff_stubReturnString() -> String {
    #if true
    fatalError()
    #endif
}

func ff_stubReturnGeneric<T>() -> T {
    #if true
    fatalError()
    #endif
}

// Trying

func ff_tryExplicit() throws -> String {
    #if true
    return try failableIdentity("shucks")
    #endif
}

func ff_tryImplicit() throws -> String {
    #if true
    try failableIdentity("howdy")
    #endif
}

func ff_tryExplicitMissingThrows() -> String {
    #if true
    return try failableIdentity("shucks") // expected-error {{errors thrown from here are not handled}}
    #endif
}

func ff_tryImplicitMissingThrows() -> String {
    #if true
    try failableIdentity("howdy") // expected-error {{errors thrown from here are not handled}}
    #endif
}

// Forced Trying

func ff_forceTryExplicit() -> String {
    #if true
    return try! failableIdentity("howdy")
    #endif
}

func ff_forceTryImplicit() -> String {
    #if true
    try! failableIdentity("shucks")
    #endif
}

func ff_forceTryExplicitAddingThrows() throws -> String {
    #if true
    return try! failableIdentity("howdy")
    #endif
}

func ff_forceTryImplicitAddingThrows() throws -> String {
    #if true
    try! failableIdentity("shucks")
    #endif
}

// Optional Trying

func ff_optionalTryExplicit() -> String? {
    #if true
    return try? failableIdentity("howdy")
    #endif
}

func ff_optionalTryImplicit() -> String? {
    #if true
    try? failableIdentity("shucks")
    #endif
}

func ff_optionalTryExplicitAddingThrows() throws -> String? {
    #if true
    return try? failableIdentity("shucks")
    #endif
}

func ff_optionalTryImplicitAddingThrows() throws -> String? {
    #if true
    try? failableIdentity("howdy")
    #endif
}

// Inferred Return Types

func ff_inferredIntegerLiteralInt() -> Int {
    #if true
    0
    #endif
}

func ff_inferredIntegerLiteralInt8() -> Int8 {
    #if true
    0
    #endif
}

func ff_inferredIntegerLiteralInt16() -> Int16 {
    #if true
    0
    #endif
}

func ff_inferredIntegerLiteralInt32() -> Int32 {
    #if true
    0
    #endif
}

func ff_inferredIntegerLiteralInt64() -> Int64 {
    #if true
    0
    #endif
}

func ff_inferredIntegerLiteralMyOwnInt() -> MyOwnInt {
    #if true
    0
    #endif
}

func ff_nilLiteralInt() -> Int? {
    #if true
    nil
    #endif
}

func ff_inferredFloatLiteralFloat() -> Float {
    #if true
    0.0
    #endif
}

func ff_inferredFloatLiteralDouble() -> Double {
    #if true
    0.0
    #endif
}

func ff_inferredFloatLiteralMyOwnDouble() -> MyOwnFloat {
    #if true
    0.0
    #endif
}

func ff_inferredBooleanLiteralBool() -> Bool {
    #if true
    true
    #endif
}

func ff_inferredBooleanLiteralMyOwnBoolean() -> MyOwnBoolean {
    #if true
    true
    #endif
}

func ff_inferredStringLiteralString() -> String {
    #if true
    "howdy"
    #endif
}

func ff_inferredStringLiteralMyOwnString() -> MyOwnString {
    #if true
    "howdy"
    #endif
}

func ff_inferredInterpolatedStringLiteralString() -> String {
    #if true
    "\(0) \(1)"
    #endif
}

func ff_inferredInterpolatedStringLiteralString() -> MyOwnInterpolation {
    #if true
    "\(0) \(1)"
    #endif
}

func ff_inferredMagicFile() -> StaticString {
    #if true
    #file
    #endif
}

func ff_inferredMagicLine() -> UInt {
    #if true
    #line // expected-error {{#line directive was renamed to #sourceLocation}}
    #endif // expected-error {{parameterless closing #sourceLocation() directive without prior opening #sourceLocation(file:,line:) directive}}
}

func ff_inferredMagicColumn() -> UInt {
    #if true
    #column
    #endif
}

func ff_inferredMagicFunction() -> StaticString {
    #if true
    #function
    #endif
}

func ff_inferredMagicDSOHandle() -> UnsafeRawPointer {
    #if true
    #dsohandle
    #endif
}

func ff_implicitDiscardExpr() {
    #if true
    _ = 3
    #endif
}

func ff_implicitMetatype() -> String.Type {
    #if true
    String.self
    #endif
}

func ff_implicitMemberRef(_ instance: StructWithProperty) -> Int {
    #if true
    instance.foo
    #endif
}

func ff_implicitDynamicMember(_ s: DynamicStruct) -> String {
    #if true
    s.foo
    #endif
}

func ff_implicitParenExpr() -> Int {
    #if true
    (3 + 5)
    #endif
}

func ff_implicitTupleExpr() -> (Int, Int) {
    #if true
    (3, 5)
    #endif
}

func ff_implicitArrayExprArray() -> [Int] {
    #if true
    [1, 3, 5]
    #endif
}

func ff_implicitArrayExprSet() -> Set<Int> {
    #if true
    [1, 3, 5]
    #endif
}

func ff_implicitArrayExprMyOwnArray() -> MyOwnArray<Int> {
    #if true
    [1, 3, 5]
    #endif
}

func ff_implicitDictionaryExprDictionary() -> [Int : Int] {
    #if true
    [1 : 1, 2 : 2]
    #endif
}

func ff_implicitDictionaryExprMyOwnDictionary() -> MyOwnDictionary<Int, Int> {
    #if true
    [1 : 1, 2 : 2]
    #endif
}

func ff_implicitSubscriptExpr(_ s: SubscriptableStruct) -> Int {
    #if true
    s[13]
    #endif
}

func ff_implicitKeyPathExprWritableKeyPath() -> WritableKeyPath<Int, Int> {
    #if true
    \Int.self
    #endif
}

func ff_implicitKeyPathExprKeyPath() -> WritableKeyPath<Int, Int> {
    #if true
    \Int.self.self
    #endif
}

func ff_implicitTupleElementExpr() -> Int {
    #if true
    (1,field:2).field
    #endif
}

func ff_implicitBindExpr(_ opt: Int?) -> Int? {
    #if true
    opt?.zero
    #endif
}

func ff_implicitOptionalEvaluation(_ opt: Int?) -> Int? {
    #if true
    (opt?.zero.zero).someZero
    #endif
}

func ff_implicitForceValue(_ opt: Int?) -> Int {
    #if true
    opt!
    #endif
}

func ff_implicitTemporarilyEscapableExpr(_ cl: () -> Void) -> () -> Void {
    #if true
    withoutActuallyEscaping(cl) { $0 }
    #endif
}

func ff_implicitOpenExistentialExpr(_ f: SomeProto) -> String {
    #if true
    f.foo()
    #endif
}

func ff_implicitInjectIntoOptionalExpr(_ int: Int) -> Int? {
    #if true
    int
    #endif
}

func ff_implicitTupleShuffle(_ input: (one: Int, two: Int)) -> (two: Int, one: Int) {
    #if true
    input // expected-warning {{expression shuffles the elements of this tuple; this behavior is deprecated}}
    #endif
}

func ff_implicitCollectionUpcast(_ derived: [Derived]) -> [Base] {
    #if true
    derived
    #endif
}

func ff_implicitErasureExpr(_ conformer: SomeProtoConformer) -> SomeProto {
    #if true
    conformer
    #endif
}

func ff_implicitAnyHashableErasureExpr(_ int: Int) -> AnyHashable {
    #if true
    int
    #endif
}

func ff_implicitCallExpr<Input, Output>(input: Input, function: (Input) -> Output) -> Output {
    #if true
    function(input)
    #endif
}

func ff_implicitPrefixUnaryOperator(int: Int) -> Int {
    #if true
    -int
    #endif
}

func ff_implicitBinaryOperator(lhs: Int, rhs: Int) -> Int {
    #if true
    lhs - rhs
    #endif
}

func ff_implicitConstructorCallRefExpr(lhs: Int, rhs: Int) -> Int {
    #if true
    Int()
    #endif
}

func ff_implicitIsExpr<T>(t: T) -> Bool {
    #if true
    t is Int
    #endif
}

func ff_implicitCoerceExpr<T>() -> T.Type {
    #if true
    T.self as T.Type
    #endif
}

func ff_implicitConditionalCheckedCastExprAs<T>(t: T) -> Int? {
    #if true
    t as? Int
    #endif
}

func ff_implicitForceCheckedCastExpr<T>(t: T) -> Int {
    #if true
    t as! Int
    #endif
}

func ff_conditional(_ condition: Bool) -> Int {
    #if true
    condition ? 1 : -1
    #endif
}

var __ff_implicitAssignExpr: Int = 0
func ff_implicitAssignExpr(newValue: Int) -> Void {
    #if true
    __ff_implicitAssignExpr = newValue
    #endif
}

func ff_implicitMemberAccessInit() -> Initable {
    #if true
    Initable.init()
    #endif
}

func ff_implicitMemberAccessEnumCase() -> Unit {
    #if true
    Unit.only
    #endif
}



// MARK: - Free Properties : Implicit Get



var fv_nop: () {
    #if true
    #endif
}

var fv_nop_false: () {
  #if false
  #endif
}

var fv_missing: String {
    #if true
    #endif
}


var fv_missing_test: String {
    get {}
} 

var fv_implicit: String {
    #if true
    "hello"
    #endif
}

var fv_explicit: String {
    #if true
    return "hello"
    #endif
}

var fv_explicitClosure: () -> Void {
    #if true
    return { print("howdy") }
    #endif
}

var fv_implicitClosure: () -> Void {
    #if true
    { print("howdy") }
    #endif
}

var fv_explicitMultilineClosure: () -> String {
    #if true
    return {
        let one = "big a"
        let two = "little a"
        return "\(one) + \(two)"
    }
    #endif
}

var fv_implicitMultilineClosure: () -> String {
    #if true
    {
        let one = "big a"
        let two = "little a"
        return "\(one) + \(two)"
    }
    #endif
}

var fv_implicitWrong: String {
    #if true
    17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
    #endif
}

var fv_explicitWrong: String {
    #if true
    return 17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
    #endif
}

var fv_implicitMulti: String {
    #if true
    print("uh oh")
    "shucks howdy" // expected-warning {{string literal is unused}}
    #endif
}

var fv_explicitMulti: String {
    #if true
    print("okay")
    return "all right"
    #endif
}

var fv_effectfulUsed: String {
    #if true
    logAndReturn("okay")
    #endif
}

// Unused returns

var fv_effectfulIgnored: () {
    #if true
    logAndReturn("okay")
    #endif
}

var fv_effectfulIgnoredVoid: Void {
    #if true
    logAndReturn("okay")
    #endif
}

var fv_effectfulIgnoredSwiftVoid: Swift.Void {
    #if true
    logAndReturn("okay")
    #endif
}

// Stubs

var fv_stubEmptyTuple: () {
    #if true
    fatalError()
    #endif
}

var fv_stubVoid: Void {
    #if true
    fatalError()
    #endif
}

var fv_stubSwiftVoid: Swift.Void {
    #if true
    fatalError()
    #endif
}

var fv_stubMyVoidTypealias: MyOwnVoid {
    #if true
    fatalError()
    #endif
}

var fv_stubImplicitReturnNever: Never {
    #if true
    fatalError()
    #endif
}

var fv_stubExplicitReturnNever: Never {
    #if true
    return fatalError()
    #endif
}

var fv_stubExplicitReturnNeverAsMyOwnNever: MyOwnNever {
    #if true
    return fatalError() // expected-error {{cannot convert return expression of type 'Never' to return type 'MyOwnNever'}}
    #endif
}

var fv_stubExplicitReturnMyOwnNeverAsNever: Never {
    #if true
    return myOwnFatalError() // expected-error {{cannot convert return expression of type 'MyOwnNever' to return type 'Never'}}
    #endif
}

var fv_stubImplicitReturnNeverAsMyOwnNever: MyOwnNever {
    #if true
    fatalError()
    #endif
}

var fv_stubImplicitReturnMyOwnNeverAsNever: Never {
    #if true
    myOwnFatalError()
    #endif
}

var fv_stubString: String {
    #if true
    fatalError()
    #endif
}

// Forced Trying

var fv_forceTryUnusedExplicit: () {
    #if true
    return try! failableLogAndReturn("oh") //expected-error {{unexpected non-void return value in void function}}
    #endif
}

var fv_forceTryUnusedImplicit: () {
    #if true
    try! failableLogAndReturn("uh") 
    #endif
}

var fv_forceTryExplicit: String {
    #if true
    return try! failableIdentity("shucks")
    #endif
}

var fv_forceTryImplicit: String {
    #if true
    try! failableIdentity("howdy")
    #endif
}

// Optional Trying

var fv_optionalTryUnusedExplicit: () {
    #if true
    return try? failableLogAndReturn("uh")  //expected-error {{unexpected non-void return value in void function}}
    #endif
}

var fv_optionalTryUnusedImplicit: () {
    #if true
    try? failableLogAndReturn("oh")
    #endif
}

var fv_optionalTryExplicit: String? {
    #if true
    return try? failableIdentity("shucks")
    #endif
}

var fv_optionalTryImplicit: String? {
    #if true
    try? failableIdentity("howdy")
    #endif
}



// MARK: - Free Properties : Get



var fvg_nop: () {
    get {
        #if true
        #endif
    }
}

var fvg_nop_false: () {
    get {
        #if false
        #endif
    }
}


var fvg_missing: String {
    get {
        #if true
        #endif
    }
}

var fvg_implicit: String {
    get {
        #if true
        "hello"
        #endif
    }
}

var fvg_explicit: String {
    get {
        #if true
        return "hello"
        #endif
    }
}

var fvg_explicitClosure: () -> Void {
    get {
        #if true
        return { print("howdy") }
        #endif
    }
}

var fvg_implicitClosure: () -> Void {
    get {
        #if true
        {
            #if true
            print("howdy")
            #endif
        }
        #endif
    }
}

var fvg_explicitMultilineClosure: () -> String {
    get {
    #if true
        return {
            let one = "big a"
            let two = "little a"
            return "\(one) + \(two)"
        }
    #endif
    }
}

var fvg_implicitMultilineClosure: () -> String {
    get {
        #if true
        {
            let one = "big a"
            let two = "little a"
            return "\(one) + \(two)"
        }
        #endif
    }
}

var fvg_implicitWrong: String {
    get {
        #if true
        17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
        #endif
    }
}

var fvg_explicitWrong: String {
    get {
        #if true
        return 17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
        #endif
    }
}

var fvg_implicitMulti: String {
    get {
        #if true
        print("uh oh")
        "shucks howdy" // expected-warning {{string literal is unused}}
        #endif
    }
}

var fvg_explicitMulti: String {
    get {
        #if true
        print("okay")
        return "all right"
        #endif
    }
}

var fvg_effectfulUsed: String {
    get {
        #if true
        logAndReturn("okay")
        #endif
    }
}

// Unused returns

var fvg_effectfulIgnored: () {
    get {
        #if true
        logAndReturn("okay")
        #endif
    }
}

var fvg_effectfulIgnoredVoid: Void {
    get {
        #if true
        logAndReturn("okay")
        #endif
    }
}

var fvg_effectfulIgnoredSwiftVoid: Swift.Void {
    get {
        #if true
        logAndReturn("okay")
        #endif
    }
}

// Stubs

var fvg_stubEmptyTuple: () {
    get {
        #if true
        fatalError()
        #endif
    }
}

var fvg_stubVoid: Void {
    get {
        #if true
        fatalError()
        #endif
    }
}

var fvg_stubSwiftVoid: Swift.Void {
    get {
        #if true
        fatalError()
        #endif
    }
}

var fvg_stubMyVoidTypealias: MyOwnVoid {
    get {
        #if true
        fatalError()
        #endif
    }
}

var fvg_stubImplicitReturnNever: Never {
    get {
        #if true
        fatalError()
        #endif
    }
}

var fvg_stubExplicitReturnNever: Never {
    get {
        #if true
        return fatalError()
        #endif
    }
}

var fvg_stubExplicitReturnNeverAsMyOwnNever: MyOwnNever {
    get {
        #if true
        return fatalError() // expected-error {{cannot convert return expression of type 'Never' to return type 'MyOwnNever'}}
        #endif
    }
}

var fvg_stubExplicitReturnMyOwnNeverAsNever: Never {
    get {
        #if true
        return myOwnFatalError() // expected-error {{cannot convert return expression of type 'MyOwnNever' to return type 'Never'}}
        #endif
    }
}

var fvg_stubImplicitReturnNeverAsMyOwnNever: MyOwnNever {
    get {
        #if true
        fatalError()
        #endif
    }
}

var fvg_stubImplicitReturnMyOwnNeverAsNever: Never {
    get {
        #if true
        myOwnFatalError()
        #endif
    }
}

var fvg_stubString: String {
    get {
        #if true
        fatalError()
        #endif
    }
}

// Forced Trying

var fvg_forceTryExplicit: String {
    get {
        #if true
        return try! failableIdentity("shucks")
        #endif
    }
}

var fvg_forceTryImplicit: String {
    get {
        #if true
        try! failableIdentity("howdy")
        #endif
    }
}

// Optional Trying

var fvg_optionalTryExplicit: String? {
    get {
        #if true
        return try? failableIdentity("shucks")
        #endif
    }
}

var fvg_optionalTryImplicit: String? {
    get {
        #if true
        try? failableIdentity("howdy")
        #endif
    }
}



// MARK: - Free Properties : Set



var fvs_nop: () {
    get {
        #if true
        #endif
    }
    set {
        #if true
        #endif
    }
}

var fvs_nop_false: () {
    get {
        #if false
        #endif
    }
    set {
        #if false
        #endif
    }
}

var fvs_implicit: String {
    get {
        #if true
        "ok"
        #endif
    }
    set {
        #if true
        "hello" // expected-warning {{string literal is unused}} 
        #endif
    }
}

var fvs_explicit: String {
    get {
        #if true
        "ok"
        #endif
    }
    set {
        #if true
        return "hello" // expected-error {{unexpected non-void return value in void function}}
        #endif
    }
}

var fvs_explicitClosure: () -> Void {
    get {
        #if true
        return {
            #if true
            print("howdy")
            #endif
        }
        #endif
    }
    set {
        #if true
        return { // expected-error {{unexpected non-void return value in void function}}
            #if true
            print("howdy")
            #endif
        }
        #endif
    }
}

var fvs_implicitClosure: () -> Void {
    get {
        #if true
        {
            #if true
            print("howdy")
            #endif
        }
        #endif
    }
    set {
        #if true
        { // expected-error {{closure expression is unused}} expected-note {{did you mean to use a 'do' statement?}}
            #if true
            print("howdy")
            #endif
        }
        #endif
    }
}

var fvs_implicitWrong: String {
    get {
        #if true
        "ok"
        #endif
    }
    set {
        #if true
        17 // expected-warning {{integer literal is unused}}
        #endif
    }
}

var fvs_explicitWrong: String {
    get {
        #if true
        "ok"
        #endif
    }
    set {
        #if true
        return 17 // expected-error {{unexpected non-void return value in void function}}
        #endif
    }
}

var fvs_implicitMulti: String {
    get {
        #if true
        "ok"
        #endif
    }
    set {
        #if true
        print("uh oh")
        "shucks howdy" // expected-warning {{string literal is unused}}
        #endif
    }
}

var fvs_explicitMulti: String {
    get {
        #if true
        "ok"
        #endif
    }
    set {
        #if true
        print("okay")
        return "all right" // expected-error {{unexpected non-void return value in void function}}
        #endif
    }
}

var fvs_effectfulUsed: String {
    get {
        #if true
        "ok"
        #endif
    }
    set {
        #if true
        logAndReturn("okay")
        #endif
    }
}

// Stubs

var fvs_stub: () {
    get {
        #if true
        ()
        #endif
    }
    set {
        #if true
        fatalError()
        #endif
    }
}

var fvs_stubMyOwnFatalError: () {
    get {
        #if true
        ()
        #endif
    }
    set {
        #if true
        myOwnFatalError()
        #endif
    }
}
// Forced Trying

var fvs_forceTryExplicit: String {
    get {
        #if true
        "ok"
        #endif
    }
    set {
        #if true
        return try! failableIdentity("shucks") // expected-error {{unexpected non-void return value in void function}}
        #endif
    }
}

var fvs_forceTryImplicit: String {
    get {
        #if true
        "ok"
        #endif
    }
    set {
        #if true
        try! failableIdentity("howdy") // expected-warning {{result of call to 'failableIdentity' is unused}}
        #endif
    }
}

// Optional Trying

var fvs_optionalTryExplicit: String? {
    get {
        #if true
        "ok"
        #endif
    }
    set {
        #if true
        return try? failableIdentity("shucks") // expected-error {{unexpected non-void return value in void function}}
        #endif
    }
}

var fvs_optionalTryImplicit: String? {
    get {
        #if true
        "ok"
        #endif
    }
    set {
        #if true
        try? failableIdentity("howdy") // expected-warning {{result of 'try?' is unused}}
        #endif
    }
}



// MARK: - Free Properties : Read






// MARK: - Free Properties : Modify






// MARK: - Subscripts : Implicit Readonly



enum S_nop {
    subscript() -> () {
        #if true
        #endif
    }
}

enum S_nop_false {
    subscript() -> () {
        #if false
        #endif
    }
}

enum S_missing {
    subscript() -> String { 
        #if true
        #endif
    }
}

enum S_implicit {
    subscript() -> String {
        #if true
        "hello"
        #endif
    }
}

enum S_explicit {
    subscript() -> String {
        #if true
        return "hello"
        #endif
    }
}

enum S_explicitClosure {
    subscript() -> () -> Void {
        #if true
        return { print("howdy") } 
        #endif
    }
}

enum S_implicitClosure {
    subscript() -> () -> Void {
        #if true
        {
            #if true
            print("howdy")
            #endif
        }
        #endif
    }
}

enum S_explicitMultilineClosure {
   subscript() -> () -> String {
        #if true
        return {
            let one = "big a"
            let two = "little a"
            return "\(one) + \(two)"
        }
        #endif
    }
}

enum S_implicitMultilineClosure {
    subscript() -> () -> String {
        #if true
        {
            let one = "big a"
            let two = "little a"
            return "\(one) + \(two)"
        }
        #endif
    }
}

enum S_implicitWrong {
    subscript() -> String {
        #if true
        17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
        #endif
    }
}

enum S_explicitWrong {
    subscript() -> String {
        #if true
        return 17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
        #endif
    }
}

enum S_implicitMulti {
    subscript() -> String {
        #if true
        print("uh oh")
        "shucks howdy" // expected-warning {{string literal is unused}}
        #endif
    }
}

enum S_explicitMulti {
    subscript() -> String {
        #if true
        print("okay")
        return "all right"
        #endif
    }
}

enum S_effectfulUsed {
    subscript() -> String {
        #if true
        logAndReturn("okay")
        #endif
    }
}

// Unused returns

enum S_effectfulIgnored {
    subscript() -> () {
        #if true
        logAndReturn("okay")
        #endif
    }
}

enum S_effectfulIgnoredVoid {
    subscript() -> Void {
        #if true
        logAndReturn("okay")
        #endif
    }
}

enum S_effectfulIgnoredSwiftVoid {
    subscript() -> Swift.Void {
        #if true
        logAndReturn("okay")
        #endif
    }
}

// Stubs

enum S_stubEmptyTuple {
    subscript() -> () {
        #if true
        fatalError()
        #endif
    }
}

enum S_stubVoid {
    subscript() -> Void {
        #if true
        fatalError()
        #endif
    }
}

enum S_stubSwiftVoid {
    subscript() -> Swift.Void {
        #if true
        fatalError()
        #endif
    }
}

enum S_stubMyVoidTypealias {
    subscript() -> MyOwnVoid {
        #if true
        fatalError()
        #endif
    }
}

enum S_stubImplicitReturnNever {
    subscript() -> Never {
        #if true
        fatalError()
        #endif
    }
}

enum S_stubExplicitReturnNever {
    subscript() -> Never {
        #if true
        return fatalError()
        #endif
    }
}

enum S_stubExplicitReturnNeverAsMyOwnNever {
    subscript() -> MyOwnNever {
        #if true
        return fatalError() // expected-error {{cannot convert return expression of type 'Never' to return type 'MyOwnNever'}}
        #endif
    }
}

enum S_stubExplicitReturnMyOwnNeverAsNever {
    subscript() -> Never {
        #if true
        return myOwnFatalError() // expected-error {{cannot convert return expression of type 'MyOwnNever' to return type 'Never'}}
        #endif
    }
}

enum S_stubImplicitReturnNeverAsMyOwnNever {
    subscript() -> MyOwnNever {
        #if true
        fatalError()
        #endif
    }
}

enum S_stubImplicitReturnMyOwnNeverAsNever {
    subscript() -> Never {
        #if true
        myOwnFatalError()
        #endif
    }
}

enum S_stubString {
    subscript() -> String {
        #if true
        fatalError()
        #endif
    }
}

enum S_stubGeneric {
    subscript<T>() -> T {
        #if true
        fatalError()
        #endif
    }
}

// Forced Trying

enum S_forceTryExplicit {
    subscript() -> String {
        #if true
        return try! failableIdentity("shucks")
        #endif
    }
}

enum S_forceTryImplicit {
    subscript() -> String {
        #if true
        try! failableIdentity("howdy")
        #endif
    }
}

// Optional Trying

enum S_optionalTryExplicit {
    subscript() -> String? {
        #if true
        return try? failableIdentity("shucks")
        #endif
    }
}

enum S_optionalTryImplicit {
    subscript() -> String? {
        #if true
        try? failableIdentity("howdy")
        #endif
    }
}



// MARK: - Subscripts : Explicit Readonly



enum SRO_nop {
    subscript() -> () {
        get {
            #if true
            #endif
        }
    }
}

enum SRO_nop_false {
    subscript() -> () {
        get {
            #if false
            #endif
        }
    }
}

enum SRO_missing {
    subscript() -> String {
        get {
            #if true
            #endif
        }
    }
}

enum SRO_implicit {
    subscript() -> String {
        get {
            #if true
            "hello"
            #endif
        }
    }
}

enum SRO_explicit {
    subscript() -> String {
        get {
            #if true
            return "hello"
            #endif
        }
    }
}

enum SRO_explicitClosure {
    subscript() -> () -> Void {
        get {
            #if true
            return {
                #if true
                print("howdy")
                #endif
            } 
            #endif
        }
    }
}

enum SRO_implicitClosure {
    subscript() -> () -> Void {
        get {
            #if true
            {
                #if true
                print("howdy")
                #endif
            }
            #endif
        }
    }
}

enum SRO_explicitMultilineClosure {
   subscript() -> () -> String {
        get {
            #if true
            return {
                let one = "big a"
                let two = "little a"
                return "\(one) + \(two)"
            }
            #endif
        }
    }
}

enum SRO_implicitMultilineClosure {
    subscript() -> () -> String {
        get {
            #if true
            {
                let one = "big a"
                let two = "little a"
                return "\(one) + \(two)"
            }
            #endif
        }
    }
}

enum SRO_implicitWrong {
    subscript() -> String {
        get {
            #if true
            17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
            #endif
        }
    }
}

enum SRO_explicitWrong {
    subscript() -> String {
        get {
            #if true
            return 17 // expected-error {{cannot convert return expression of type 'Int' to return type 'String'}}
            #endif
        }
    }
}

enum SRO_implicitMulti {
    subscript() -> String {
        get {
            #if true
            print("uh oh")
            "shucks howdy" // expected-warning {{string literal is unused}}
            #endif
        }
    }
}

enum SRO_explicitMulti {
    subscript() -> String {
        get {
            #if true
            print("okay")
            return "all right"
            #endif
        }
    }
}

enum SRO_effectfulUsed {
    subscript() -> String {
        get {
            #if true
            logAndReturn("okay")
            #endif
        }
    }
}

// Unused returns

enum SRO_effectfulIgnored {
    subscript() -> () {
        get {
            #if true
            logAndReturn("okay")
            #endif
        }
    }
}

enum SRO_effectfulIgnoredVoid {
    subscript() -> Void {
        get {
            #if true
            logAndReturn("okay")
            #endif
        }
    }
}

enum SRO_effectfulIgnoredSwiftVoid {
    subscript() -> Swift.Void {
        get {
            #if true
            logAndReturn("okay")
            #endif
        }
    }
}

// Stubs

enum SRO_stubEmptyTuple {
    subscript() -> () {
        get {
            #if true
            fatalError()
            #endif
        }
    }
}

enum SRO_stubVoid {
    subscript() -> Void {
        get {
            #if true
            fatalError()
            #endif
        }
    }
}

enum SRO_stubSwiftVoid {
    subscript() -> Swift.Void {
        get {
            #if true
            fatalError()
            #endif
        }
    }
}

enum SRO_stubMyVoidTypealias {
    subscript() -> MyOwnVoid {
        get {
            #if true
            fatalError()
            #endif
        }
    }
}

enum SRO_stubImplicitReturnNever {
    subscript() -> Never {
        get {
            #if true
            fatalError()
            #endif
        }
    }
}

enum SRO_stubExplicitReturnNever {
    subscript() -> Never {
        get {
            #if true
            return fatalError()
            #endif
        }
    }
}

enum SRO_stubExplicitReturnNeverAsMyOwnNever {
    subscript() -> MyOwnNever {
        get {
            #if true
            return fatalError() // expected-error {{cannot convert return expression of type 'Never' to return type 'MyOwnNever'}}
            #endif
        }
    }
}

enum SRO_stubExplicitReturnMyOwnNeverAsNever {
    subscript() -> Never {
        get {
            #if true
            return myOwnFatalError() // expected-error {{cannot convert return expression of type 'MyOwnNever' to return type 'Never'}}
            #endif
        }
    }
}

enum SRO_stubImplicitReturnNeverAsMyOwnNever {
    subscript() -> MyOwnNever {
        get {
            #if true
            fatalError()
            #endif
        }
    }
}

enum SRO_stubImplicitReturnMyOwnNeverAsNever {
    subscript() -> Never {
        get {
            #if true
            myOwnFatalError()
            #endif
        }
    }
}

enum SRO_stubString {
    subscript() -> String {
        get {
            #if true
            fatalError()
            #endif
        }
    }
}

enum SRO_stubGeneric {
    subscript<T>() -> T {
        get {
            #if true
            fatalError()
            #endif
        }
    }
}

// Forced Trying

enum SRO_forceTryExplicit {
    subscript() -> String {
        get {
            #if true
            return try! failableIdentity("shucks")
            #endif
        }
    }
}

enum SRO_forceTryImplicit {
    subscript() -> String {
        get {
            #if true
            try! failableIdentity("howdy")
            #endif
        }
    }
}

// Optional Trying

enum SRO_optionalTryExplicit {
    subscript() -> String? {
        get {
            #if true
            return try? failableIdentity("shucks")
            #endif
        }
    }
}

enum SRO_optionalTryImplicit {
    subscript() -> String? {
        get {
            #if true
            try? failableIdentity("howdy")
            #endif
        }
    }
}



// MARK: - Subscripts : Set






// MARK: - Subscripts : Read/Modify






// MARK: - Constructors



struct C_nop {
    init() {
        #if true
        #endif
    }
}

struct C_nop_false {
    init() {
        #if false
        #endif
    }
}

struct C_missing {
    var i: Int
    init?() {
        #if true
        #endif
    }
}

struct C_implicitNil {
    init?() {
        #if true
        nil
        #endif
    }
}

struct C_explicitNil {
    init?() {
        #if true
        return nil
        #endif
    }
}

struct C_forcedMissing {
    var i: Int
    init!() {
        #if true
        #endif
    }
}

struct C_forcedImplicitNil {
    init!() {
        #if true
        nil
        #endif
    }
}

struct C_forcedExplicitNil {
    init?() {
        #if true
        return nil
        #endif
    }
}

struct C_implicit {
    init() {
        #if true
        "hello" // expected-warning {{string literal is unused}}
        #endif
    }
}

struct C_explicit {
    init() {
        #if true
        return "hello" // expected-error {{'nil' is the only return value permitted in an initializer}}
        #endif
    }
}



// MARK: - Destructors



class D_nop {
    deinit {
        #if true
        #endif
    }
}

class D_nop_false {
    deinit {
        #if false
        #endif
    }
}

class D_implicit {
    deinit {
        #if true
        "bye now" // expected-warning {{string literal is unused}}
        #endif
    }
}

class D_explicit {
    deinit {
        #if true
        return "bye now" // expected-error {{unexpected non-void return value in void function}}
        #endif
    }
}

class D_implicitMulti {
    deinit {
        #if true
        print("okay")
        "see ya" // expected-warning {{string literal is unused}}
        #endif
    }
}

class D_explicitMulti {
    deinit {
        #if true
        print("okay")
        return "see ya" // expected-error {{unexpected non-void return value in void function}}
        #endif
    }
}

// Unused returns

class D_effectfulIgnored {
    deinit {
        #if true
        logAndReturn("bye now")
        #endif
    }
}

// Stubs

class D_stub {
    deinit {
        #if true
        fatalError()
        #endif
    }
}

class D_stubMyOwnDeinit {
    deinit {
        #if true
        myOwnFatalError()
        #endif
    }
}

// Forced Trying

class D_forceTryUnusedExplicit {
    deinit {
        #if true
        return try! failableLogAndReturn("uh") // expected-error {{unexpected non-void return value in void function}}
        #endif
    }
}

class D_forceTryUnusedImplicit {
    deinit {
        #if true
        try! failableLogAndReturn("oh")
        #endif
    }
}

// Optional Trying

class D_optionalTryUnusedExplicit {
    deinit {
        #if true
        return try? failableLogAndReturn("uh") // expected-error {{unexpected non-void return value in void function}}
        #endif
    }
}

class D_optionalTryUnusedImplicit {
    deinit {
        #if true
        try? failableLogAndReturn("oh")
        #endif
    }
}



// Miscellaneous

class CSuperExpr_Base { init() {} }
class CSuperExpr_Derived : CSuperExpr_Base { override init() { super.init() } }

class CImplicitIdentityExpr { func gimme() -> CImplicitIdentityExpr { self } }

class CImplicitDotSelfExpr { func gimme() -> CImplicitDotSelfExpr { self.self } }

func badIs<T>(_ value: Any, anInstanceOf type: T.Type) -> Bool { // expected-note {{'type' declared here}}
    #if true
    value is type // expected-error {{type-casting operator expects a type on its right-hand side (got: parameter 'type')}}
    #endif
}



// Autoclosure Discriminators



func embedAutoclosure_standard() -> Int {
    #if true
    _helpEmbedAutoclosure_standard(42)
    #endif
}
func _helpEmbedAutoclosure_standard<T>(_ value: @autoclosure () -> T) -> T {
    #if true
    value()
    #endif
}

func embedAutoclosure_never() -> Int {
    #if true
    fatalError("unsupported")
    #endif
}


// MARK: - If Declaration Variations

var double_nested_ifdecl: Int {
    #if true
    #if true
    0
    #endif
    #endif
}

var triple_nested_ifdecl: Int {
    #if true
    #if true
    #if true
    0
    #endif
    #endif
    #endif
}

var false_ifdecl: Int {
    #if false
    0
    #else
    1
    #endif
}

var false_nested_ifdecl: Int {
    #if false
    0
    #else
    1
    #endif
}

var mismatched_explicit_return_ifdecl: Int {
    #if true
    return 0
    #else
    1
    #endif
}

var mismatched_implicit_return_ifdecl: Int {
    #if true
    0
    #else
    return 1
    #endif
}

enum VerticalDirection {
    case up, down
}
func implicitMember() -> VerticalDirection {
    #if false
      .up
    #elseif true
      .down
    #else
      .unknown
    #endif
}
