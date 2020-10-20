// RUN: %target-typecheck-verify-swift

@_functionBuilder // expected-error {{'@_functionBuilder' attribute cannot be applied to this declaration}}
var globalBuilder: Int

@_functionBuilder // expected-error {{'@_functionBuilder' attribute cannot be applied to this declaration}}
func globalBuilderFunction() -> Int { return 0 }

@_functionBuilder
struct Maker {} // expected-error {{function builder must provide at least one static 'buildBlock' method}}

@_functionBuilder
class Inventor {} // expected-error {{function builder must provide at least one static 'buildBlock' method}}

@Maker // expected-error {{function builder attribute 'Maker' can only be applied to a parameter, function, or computed property}}
typealias typename = Inventor

@Maker // expected-error {{function builder attribute 'Maker' can only be applied to a variable if it defines a getter}}
var global: Int

// FIXME: should this be allowed?
@Maker
var globalWithEmptyImplicitGetter: Int {}
// expected-error@-1 {{computed property must have accessors specified}}
// expected-error@-3 {{function builder attribute 'Maker' can only be applied to a variable if it defines a getter}}

@Maker
var globalWithEmptyExplicitGetter: Int { get {} }  // expected-error{{type 'Maker' has no member 'buildBlock'}}

@Maker
var globalWithSingleGetter: Int { 0 } // expected-error {{ype 'Maker' has no member 'buildBlock'}}

@Maker
var globalWithMultiGetter: Int { 0; 0 } // expected-error {{ype 'Maker' has no member 'buildBlock'}}

@Maker
func globalFunction() {} // expected-error {{ype 'Maker' has no member 'buildBlock'}}

@Maker
func globalFunctionWithFunctionParam(fn: () -> ()) {}  // expected-error {{ype 'Maker' has no member 'buildBlock'}}

func makerParam(@Maker
                fn: () -> ()) {}

// FIXME: these diagnostics are reversed?
func makerParamRedundant(@Maker // expected-error {{only one function builder attribute can be attached to a parameter}}
                         @Maker // expected-note {{previous function builder specified here}}
                         fn: () -> ()) {}

func makerParamConflict(@Maker // expected-error {{only one function builder attribute can be attached to a parameter}}
                        @Inventor // expected-note {{previous function builder specified here}}
                        fn: () -> ()) {}

func makerParamMissing1(@Missing // expected-error {{unknown attribute 'Missing'}}
                        @Maker
                        fn: () -> ()) {}

func makerParamMissing2(@Maker
                        @Missing // expected-error {{unknown attribute 'Missing'}}
                        fn: () -> ()) {}

func makerParamExtra(@Maker(5) // expected-error {{function builder attributes cannot have arguments}}
                     fn: () -> ()) {}

func makerParamAutoclosure(@Maker // expected-error {{function builder attribute 'Maker' cannot be applied to an autoclosure parameter}}
                           fn: @autoclosure () -> ()) {}

@_functionBuilder
struct GenericMaker<T> {} // expected-note {{generic type 'GenericMaker' declared here}} expected-error {{function builder must provide at least one static 'buildBlock' method}}

struct GenericContainer<T> {  // expected-note {{generic type 'GenericContainer' declared here}}
  @_functionBuilder
  struct Maker {} // expected-error {{function builder must provide at least one static 'buildBlock' method}}
}

func makeParamUnbound(@GenericMaker // expected-error {{reference to generic type 'GenericMaker' requires arguments}}
                      fn: () -> ()) {}

func makeParamBound(@GenericMaker<Int>
                    fn: () -> ()) {}

func makeParamNestedUnbound(@GenericContainer.Maker // expected-error {{reference to generic type 'GenericContainer' requires arguments}}
                            fn: () -> ()) {}

func makeParamNestedBound(@GenericContainer<Int>.Maker
                          fn: () -> ()) {}


protocol P { }

@_functionBuilder
struct ConstrainedGenericMaker<T: P> {} // expected-error {{function builder must provide at least one static 'buildBlock' method}}


struct WithinGeneric<U> {
  func makeParamBoundInContext(@GenericMaker<U> fn: () -> ()) {}

  // expected-error@+1{{type 'U' does not conform to protocol 'P'}}
  func makeParamBoundInContextBad(@ConstrainedGenericMaker<U>
    fn: () -> ()) {}
}

@_functionBuilder
struct ValidBuilder1 {
  static func buildBlock(_ exprs: Any...) -> Int { return exprs.count }
}

protocol BuilderFuncHelper {}

extension BuilderFuncHelper {
  static func buildBlock(_ exprs: Any...) -> Int { return exprs.count }
}

@_functionBuilder
struct ValidBuilder2: BuilderFuncHelper {}

class BuilderFuncBase {
  static func buildBlock(_ exprs: Any...) -> Int { return exprs.count }
}

@_functionBuilder
class ValidBuilder3: BuilderFuncBase {}

@_functionBuilder
struct ValidBuilder4 {}
extension ValidBuilder4 {
    static func buildBlock(_ exprs: Any...) -> Int { return exprs.count }
}

@_functionBuilder
struct ValidBuilder5 {
    static func buildBlock() -> Int { 0 }
}

@_functionBuilder
struct InvalidBuilder1 {} // expected-error {{function builder must provide at least one static 'buildBlock' method}}

@_functionBuilder
struct InvalidBuilder2 { // expected-error {{function builder must provide at least one static 'buildBlock' method}}
  func buildBlock(_ exprs: Any...) -> Int { return exprs.count } // expected-note {{did you mean to make instance method 'buildBlock' static?}} {{3-3=static }}
}

@_functionBuilder
struct InvalidBuilder3 { // expected-error {{function builder must provide at least one static 'buildBlock' method}}
  var buildBlock: (Any...) -> Int = { return $0.count } // expected-note {{potential match 'buildBlock' is not a static method}}
}

@_functionBuilder
struct InvalidBuilder4 {} // expected-error {{function builder must provide at least one static 'buildBlock' method}}
extension InvalidBuilder4 {
  func buildBlock(_ exprs: Any...) -> Int { return exprs.count } // expected-note {{did you mean to make instance method 'buildBlock' static?}} {{3-3=static }}
}

protocol InvalidBuilderHelper {}
extension InvalidBuilderHelper {
  func buildBlock(_ exprs: Any...) -> Int { return exprs.count } // expected-note {{potential match 'buildBlock' is not a static method}}
}

@_functionBuilder
struct InvalidBuilder5: InvalidBuilderHelper {} // expected-error {{function builder must provide at least one static 'buildBlock' method}}

@_functionBuilder
struct InvalidBuilder6 { // expected-error {{function builder must provide at least one static 'buildBlock' method}}
    static var buildBlock: Int = 0 // expected-note {{potential match 'buildBlock' is not a static method}}
}

struct Callable {
    func callAsFunction(_ exprs: Any...) -> Int { return exprs.count }
}

@_functionBuilder
struct InvalidBuilder7 { // expected-error {{function builder must provide at least one static 'buildBlock' method}}
    static var buildBlock = Callable() // expected-note {{potential match 'buildBlock' is not a static method}}
}

class BuilderVarBase {
  static var buildBlock: (Any...) -> Int = { return $0.count } // expected-note {{potential match 'buildBlock' is not a static method}}
}

@_functionBuilder
class InvalidBuilder8: BuilderVarBase {} // expected-error {{function builder must provide at least one static 'buildBlock' method}}

protocol BuilderVarHelper {}

extension BuilderVarHelper {
  static var buildBlock: (Any...) -> Int { { return $0.count } } // expected-note {{potential match 'buildBlock' is not a static method}}
}

@_functionBuilder
struct InvalidBuilder9: BuilderVarHelper {} // expected-error {{function builder must provide at least one static 'buildBlock' method}}

@_functionBuilder
struct InvalidBuilder10 { // expected-error {{function builder must provide at least one static 'buildBlock' method}}
  static var buildBlock: (Any...) -> Int = { return $0.count } // expected-note {{potential match 'buildBlock' is not a static method}}
}

@_functionBuilder
enum InvalidBuilder11 { // expected-error {{function builder must provide at least one static 'buildBlock' method}}
    case buildBlock(Any) // expected-note {{enum case 'buildBlock' cannot be used to satisfy the function builder requirement}}
}

@_functionBuilder
struct InvalidBuilder12 { // expected-error {{function builder must provide at least one static 'buildBlock' method}}
  static func buildBlock(exprs: Any...) -> Int { return exprs.count } // expected-note {{potential match 'buildBlock' can never be called implicitly because it has at least one labeled argument}}
}

struct S {
  @ValidBuilder1 var v1: Int { 1 }
  @ValidBuilder2 var v2: Int { 1 }
  @ValidBuilder3 var v3: Int { 1 }
  @ValidBuilder4 var v4: Int { 1 }
  @ValidBuilder5 func v5() -> Int {}
  @InvalidBuilder1 var i1: Int { 1 } // expected-error {{type 'InvalidBuilder1' has no member 'buildBlock'}}
  @InvalidBuilder2 var i2: Int { 1 } // expected-error {{instance member 'buildBlock' cannot be used on type 'InvalidBuilder2'; did you mean to use a value of this type instead?}}
  @InvalidBuilder3 var i3: Int { 1 } // expected-error {{instance member 'buildBlock' cannot be used on type 'InvalidBuilder3'; did you mean to use a value of this type instead?}}
  @InvalidBuilder4 var i4: Int { 1 } // expected-error {{instance member 'buildBlock' cannot be used on type 'InvalidBuilder4'; did you mean to use a value of this type instead?}}
  @InvalidBuilder5 var i5: Int { 1 } // expected-error {{instance member 'buildBlock' cannot be used on type 'InvalidBuilder5'; did you mean to use a value of this type instead?}}
  @InvalidBuilder6 var i6: Int { 1 } // expected-error {{cannot call value of non-function type 'Int'}}
  @InvalidBuilder7 var i7: Int { 1 }
  @InvalidBuilder8 var i8: Int { 1 }
  @InvalidBuilder9 var i9: Int { 1 }
  @InvalidBuilder10 var i10: Int { 1 }
  @InvalidBuilder11 var i11: InvalidBuilder11 { 1 }
  @InvalidBuilder12 var i12: Int { 1 } // expected-error {{missing argument label 'exprs:' in call}}
}
