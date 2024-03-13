// RUN: %target-typecheck-verify-swift -typo-correction-limit 100

class ThisBase1 {
  init() { }

  var baseInstanceVar: Int

  var baseProp : Int {
    get {
      return 42
    }
    set {}
  }

  func baseFunc0() {}
  func baseFunc1(_ a: Int) {}

  subscript(i: Int) -> Double {
    get {
      return Double(i)
    }
    set {
      baseInstanceVar = i
    }
  }

  class var baseStaticVar: Int = 42 // expected-error {{class stored properties not supported}}

  class var baseStaticProp: Int {
    get {
      return 42
    }
    set {}
  }

  class func baseStaticFunc0() {}

  struct BaseNestedStruct {} // expected-note {{did you mean 'BaseNestedStruct'?}}
  class BaseNestedClass {
    init() { }
  }
  enum BaseNestedUnion {
    case BaseUnionX(Int)
  }

  typealias BaseNestedTypealias = Int // expected-note {{did you mean 'BaseNestedTypealias'?}}
}

class ThisDerived1 : ThisBase1 {
  override init() { super.init() }

  var derivedInstanceVar: Int

  var derivedProp : Int {
    get {
      return 42
    }
    set {}
  }

  func derivedFunc0() {}
  func derivedFunc1(_ a: Int) {}

  subscript(i: Double) -> Int {
    get {
      return Int(i)
    }
    set {
      baseInstanceVar = Int(i)
    }
  }

  class var derivedStaticVar: Int = 42// expected-error {{class stored properties not supported}}

  class var derivedStaticProp: Int {
    get {
      return 42
    }
    set {}
  }

  class func derivedStaticFunc0() {}

  struct DerivedNestedStruct {}
  class DerivedNestedClass {
    init() { }
  }
  enum DerivedNestedUnion { // expected-note {{did you mean 'DerivedNestedUnion'?}}
    case DerivedUnionX(Int)
  }

  typealias DerivedNestedTypealias = Int

  func testSelf1() {
    self.baseInstanceVar = 42
    self.baseProp = 42
    self.baseFunc0()
    self.baseFunc1(42)
    self[0] = 42.0
    self.baseStaticVar = 42 // expected-error {{static member 'baseStaticVar' cannot be used on instance of type 'ThisDerived1'}}
    self.baseStaticProp = 42 // expected-error {{static member 'baseStaticProp' cannot be used on instance of type 'ThisDerived1'}}
    self.baseStaticFunc0() // expected-error {{static member 'baseStaticFunc0' cannot be used on instance of type 'ThisDerived1'}}

    self.baseExtProp = 42
    self.baseExtFunc0()
    self.baseExtStaticVar = 42
    self.baseExtStaticProp = 42
    self.baseExtStaticFunc0() // expected-error {{static member 'baseExtStaticFunc0' cannot be used on instance of type 'ThisDerived1'}}

    var bs1 : BaseNestedStruct
    var bc1 : BaseNestedClass
    var bo1 : BaseNestedUnion = .BaseUnionX(42)
    var bt1 : BaseNestedTypealias
    var bs2 = self.BaseNestedStruct() // expected-error{{static member 'BaseNestedStruct' cannot be used on instance of type 'ThisDerived1'}}
    var bc2 = self.BaseNestedClass() // expected-error{{static member 'BaseNestedClass' cannot be used on instance of type 'ThisDerived1'}}
    var bo2 = self.BaseUnionX(24) // expected-error {{value of type 'ThisDerived1' has no member 'BaseUnionX'}}
    var bo3 = self.BaseNestedUnion.BaseUnionX(24) // expected-error{{static member 'BaseNestedUnion' cannot be used on instance of type 'ThisDerived1'}}
    var bt2 = self.BaseNestedTypealias(42) // expected-error{{static member 'BaseNestedTypealias' cannot be used on instance of type 'ThisDerived1'}}

    var bes1 : BaseExtNestedStruct
    var bec1 : BaseExtNestedClass
    var beo1 : BaseExtNestedUnion = .BaseExtUnionX(42)
    var bet1 : BaseExtNestedTypealias
    var bes2 = self.BaseExtNestedStruct() // expected-error{{static member 'BaseExtNestedStruct' cannot be used on instance of type 'ThisDerived1'}}
    var bec2 = self.BaseExtNestedClass() // expected-error{{static member 'BaseExtNestedClass' cannot be used on instance of type 'ThisDerived1'}}
    var beo2 = self.BaseExtUnionX(24) // expected-error {{value of type 'ThisDerived1' has no member 'BaseExtUnionX'}}
    var beo3 = self.BaseExtNestedUnion.BaseExtUnionX(24) // expected-error{{static member 'BaseExtNestedUnion' cannot be used on instance of type 'ThisDerived1'}}
    var bet2 = self.BaseExtNestedTypealias(42) // expected-error{{static member 'BaseExtNestedTypealias' cannot be used on instance of type 'ThisDerived1'}}

    self.derivedInstanceVar = 42
    self.derivedProp = 42
    self.derivedFunc0()
    self.derivedStaticVar = 42 // expected-error {{static member 'derivedStaticVar' cannot be used on instance of type 'ThisDerived1'}}
    self.derivedStaticProp = 42 // expected-error {{static member 'derivedStaticProp' cannot be used on instance of type 'ThisDerived1'}}
    self.derivedStaticFunc0() // expected-error {{static member 'derivedStaticFunc0' cannot be used on instance of type 'ThisDerived1'}}

    self.derivedExtProp = 42
    self.derivedExtFunc0()
    self.derivedExtStaticVar = 42
    self.derivedExtStaticProp = 42
    self.derivedExtStaticFunc0() // expected-error {{static member 'derivedExtStaticFunc0' cannot be used on instance of type 'ThisDerived1'}}

    var ds1 : DerivedNestedStruct
    var dc1 : DerivedNestedClass
    var do1 : DerivedNestedUnion = .DerivedUnionX(42)
    var dt1 : DerivedNestedTypealias
    var ds2 = self.DerivedNestedStruct() // expected-error{{static member 'DerivedNestedStruct' cannot be used on instance of type 'ThisDerived1'}}
    var dc2 = self.DerivedNestedClass() // expected-error{{static member 'DerivedNestedClass' cannot be used on instance of type 'ThisDerived1'}}
    var do2 = self.DerivedUnionX(24) // expected-error {{value of type 'ThisDerived1' has no member 'DerivedUnionX'}}
    var do3 = self.DerivedNestedUnion.DerivedUnionX(24) // expected-error{{static member 'DerivedNestedUnion' cannot be used on instance of type 'ThisDerived1'}}
    var dt2 = self.DerivedNestedTypealias(42) // expected-error{{static member 'DerivedNestedTypealias' cannot be used on instance of type 'ThisDerived1'}}

    var des1 : DerivedExtNestedStruct
    var dec1 : DerivedExtNestedClass
    var deo1 : DerivedExtNestedUnion = .DerivedExtUnionX(42)
    var det1 : DerivedExtNestedTypealias
    var des2 = self.DerivedExtNestedStruct() // expected-error{{static member 'DerivedExtNestedStruct' cannot be used on instance of type 'ThisDerived1'}}
    var dec2 = self.DerivedExtNestedClass() // expected-error{{static member 'DerivedExtNestedClass' cannot be used on instance of type 'ThisDerived1'}}
    var deo2 = self.DerivedExtUnionX(24) // expected-error {{value of type 'ThisDerived1' has no member 'DerivedExtUnionX'}}
    var deo3 = self.DerivedExtNestedUnion.DerivedExtUnionX(24) // expected-error{{static member 'DerivedExtNestedUnion' cannot be used on instance of type 'ThisDerived1'}}
    var det2 = self.DerivedExtNestedTypealias(42) // expected-error{{static member 'DerivedExtNestedTypealias' cannot be used on instance of type 'ThisDerived1'}}

    self.Type // expected-error {{value of type 'ThisDerived1' has no member 'Type'}}
  }

  func testSuper1() {
    super.baseInstanceVar = 42
    super.baseProp = 42
    super.baseFunc0()
    super.baseFunc1(42)
    super[0] = 42.0
    super.baseStaticVar = 42 // expected-error {{static member 'baseStaticVar' cannot be used on instance of type 'ThisBase1'}}
    super.baseStaticProp = 42 // expected-error {{static member 'baseStaticProp' cannot be used on instance of type 'ThisBase1'}}
    super.baseStaticFunc0() // expected-error {{static member 'baseStaticFunc0' cannot be used on instance of type 'ThisBase1'}}

    super.baseExtProp = 42
    super.baseExtFunc0()
    super.baseExtStaticVar = 42
    super.baseExtStaticProp = 42
    super.baseExtStaticFunc0() // expected-error {{static member 'baseExtStaticFunc0' cannot be used on instance of type 'ThisBase1'}}

    var bs2 = super.BaseNestedStruct() // expected-error{{static member 'BaseNestedStruct' cannot be used on instance of type 'ThisBase1'}}
    var bc2 = super.BaseNestedClass() // expected-error{{static member 'BaseNestedClass' cannot be used on instance of type 'ThisBase1'}}
    var bo2 = super.BaseUnionX(24) // expected-error {{value of type 'ThisBase1' has no member 'BaseUnionX'}}
    var bo3 = super.BaseNestedUnion.BaseUnionX(24) // expected-error{{static member 'BaseNestedUnion' cannot be used on instance of type 'ThisBase1'}}
    var bt2 = super.BaseNestedTypealias(42) // expected-error{{static member 'BaseNestedTypealias' cannot be used on instance of type 'ThisBase1'}}

    var bes2 = super.BaseExtNestedStruct() // expected-error{{static member 'BaseExtNestedStruct' cannot be used on instance of type 'ThisBase1'}}
    var bec2 = super.BaseExtNestedClass() // expected-error{{static member 'BaseExtNestedClass' cannot be used on instance of type 'ThisBase1'}}
    var beo2 = super.BaseExtUnionX(24) // expected-error {{value of type 'ThisBase1' has no member 'BaseExtUnionX'}}
    var beo3 = super.BaseExtNestedUnion.BaseExtUnionX(24) // expected-error{{static member 'BaseExtNestedUnion' cannot be used on instance of type 'ThisBase1'}}
    var bet2 = super.BaseExtNestedTypealias(42) // expected-error{{static member 'BaseExtNestedTypealias' cannot be used on instance of type 'ThisBase1'}}

    super.derivedInstanceVar = 42 // expected-error {{value of type 'ThisBase1' has no member 'derivedInstanceVar'}}
    super.derivedProp = 42 // expected-error {{value of type 'ThisBase1' has no member 'derivedProp'}}
    super.derivedFunc0() // expected-error {{value of type 'ThisBase1' has no member 'derivedFunc0'}}
    super.derivedStaticVar = 42 // expected-error {{value of type 'ThisBase1' has no member 'derivedStaticVar'}}
    super.derivedStaticProp = 42 // expected-error {{value of type 'ThisBase1' has no member 'derivedStaticProp'}}
    super.derivedStaticFunc0() // expected-error {{value of type 'ThisBase1' has no member 'derivedStaticFunc0'}}

    super.derivedExtProp = 42 // expected-error {{value of type 'ThisBase1' has no member 'derivedExtProp'}}
    super.derivedExtFunc0() // expected-error {{value of type 'ThisBase1' has no member 'derivedExtFunc0'}}
    super.derivedExtStaticVar = 42 // expected-error {{value of type 'ThisBase1' has no member 'derivedExtStaticVar'; did you mean 'baseExtStaticVar'?}}
    super.derivedExtStaticProp = 42 // expected-error {{value of type 'ThisBase1' has no member 'derivedExtStaticProp'; did you mean 'baseExtStaticProp'?}}
    super.derivedExtStaticFunc0() // expected-error {{value of type 'ThisBase1' has no member 'derivedExtStaticFunc0'}}

    var ds2 = super.DerivedNestedStruct() // expected-error {{value of type 'ThisBase1' has no member 'DerivedNestedStruct'}}
    var dc2 = super.DerivedNestedClass() // expected-error {{value of type 'ThisBase1' has no member 'DerivedNestedClass'}}
    var do2 = super.DerivedUnionX(24) // expected-error {{value of type 'ThisBase1' has no member 'DerivedUnionX'}}
    var do3 = super.DerivedNestedUnion.DerivedUnionX(24) // expected-error {{value of type 'ThisBase1' has no member 'DerivedNestedUnion'}}
    var dt2 = super.DerivedNestedTypealias(42) // expected-error {{value of type 'ThisBase1' has no member 'DerivedNestedTypealias'}}

    var des2 = super.DerivedExtNestedStruct() // expected-error {{value of type 'ThisBase1' has no member 'DerivedExtNestedStruct'}}
    var dec2 = super.DerivedExtNestedClass() // expected-error {{value of type 'ThisBase1' has no member 'DerivedExtNestedClass'}}
    var deo2 = super.DerivedExtUnionX(24) // expected-error {{value of type 'ThisBase1' has no member 'DerivedExtUnionX'}}
    var deo3 = super.DerivedExtNestedUnion.DerivedExtUnionX(24) // expected-error {{value of type 'ThisBase1' has no member 'DerivedExtNestedUnion'}}
    var det2 = super.DerivedExtNestedTypealias(42) // expected-error {{value of type 'ThisBase1' has no member 'DerivedExtNestedTypealias'}}

    super.Type // expected-error {{value of type 'ThisBase1' has no member 'Type'}}
  }

  class func staticTestSelf1() {
    self.baseInstanceVar = 42 // expected-error {{member 'baseInstanceVar' of type 'ThisDerived1' cannot be used in static context}}
    self.baseProp = 42 // expected-error {{member 'baseProp' of type 'ThisDerived1' cannot be used in static context}}
    self.baseFunc0() // expected-error {{instance member 'baseFunc0' of type 'ThisDerived1' cannot be used in static context}}
    self.baseFunc0(ThisBase1())() // expected-error {{cannot convert value of type 'ThisBase1' to expected argument type 'ThisDerived1'}}

    self.baseFunc0(ThisDerived1())()
    self.baseFunc1(42) // expected-error {{instance member 'baseFunc1' of type 'ThisDerived1' cannot be used in static context}}
    self.baseFunc1(ThisBase1())(42) // expected-error {{cannot convert value of type 'ThisBase1' to expected argument type 'ThisDerived1'}}
    self.baseFunc1(ThisDerived1())(42)
    self[0] = 42.0 // expected-error {{instance member 'subscript' of type 'ThisDerived1' cannot be used in static context}}
    self.baseStaticVar = 42
    self.baseStaticProp = 42
    self.baseStaticFunc0()

    self.baseExtProp = 42 // expected-error {{member 'baseExtProp' of type 'ThisDerived1' cannot be used in static context}}
    self.baseExtFunc0() // expected-error {{instance member 'baseExtFunc0' of type 'ThisDerived1' cannot be used in static context}}
    self.baseExtStaticVar = 42 // expected-error {{instance member 'baseExtStaticVar' of type 'ThisDerived1' cannot be used in static context}}
    self.baseExtStaticProp = 42 // expected-error {{member 'baseExtStaticProp' of type 'ThisDerived1' cannot be used in static context}}
    self.baseExtStaticFunc0()

    var bs1 : BaseNestedStruct
    var bc1 : BaseNestedClass
    var bo1 : BaseNestedUnion = .BaseUnionX(42)
    var bt1 : BaseNestedTypealias
    var bs2 = self.BaseNestedStruct()
    var bc2 = self.BaseNestedClass()
    var bo2 = self.BaseUnionX(24) // expected-error {{type 'ThisDerived1' has no member 'BaseUnionX'}}
    var bo3 = self.BaseNestedUnion.BaseUnionX(24)
    var bt2 = self.BaseNestedTypealias()

    self.derivedInstanceVar = 42 // expected-error {{member 'derivedInstanceVar' of type 'ThisDerived1' cannot be used in static context}}
    self.derivedProp = 42 // expected-error {{member 'derivedProp' of type 'ThisDerived1' cannot be used in static context}}
    self.derivedFunc0() // expected-error {{instance member 'derivedFunc0' of type 'ThisDerived1' cannot be used in static context}}
    self.derivedFunc0(ThisBase1())() // expected-error {{cannot convert value of type 'ThisBase1' to expected argument type 'ThisDerived1'}}
    self.derivedFunc0(ThisDerived1())()
    self.derivedStaticVar = 42
    self.derivedStaticProp = 42
    self.derivedStaticFunc0()

    self.derivedExtProp = 42 // expected-error {{member 'derivedExtProp' of type 'ThisDerived1' cannot be used in static context}}
    self.derivedExtFunc0() // expected-error {{instance member 'derivedExtFunc0' of type 'ThisDerived1' cannot be used in static context}}
    self.derivedExtStaticVar = 42 // expected-error {{instance member 'derivedExtStaticVar' of type 'ThisDerived1' cannot be used in static context}}
    self.derivedExtStaticProp = 42 // expected-error {{member 'derivedExtStaticProp' of type 'ThisDerived1' cannot be used in static context}}
    self.derivedExtStaticFunc0()

    var ds1 : DerivedNestedStruct
    var dc1 : DerivedNestedClass
    var do1 : DerivedNestedUnion = .DerivedUnionX(42)
    var dt1 : DerivedNestedTypealias
    var ds2 = self.DerivedNestedStruct()
    var dc2 = self.DerivedNestedClass()
    var do2 = self.DerivedUnionX(24) // expected-error {{type 'ThisDerived1' has no member 'DerivedUnionX'}}
    var do3 = self.DerivedNestedUnion.DerivedUnionX(24)
    var dt2 = self.DerivedNestedTypealias()

    var des1 : DerivedExtNestedStruct
    var dec1 : DerivedExtNestedClass
    var deo1 : DerivedExtNestedUnion = .DerivedExtUnionX(42)
    var det1 : DerivedExtNestedTypealias
    var des2 = self.DerivedExtNestedStruct()
    var dec2 = self.DerivedExtNestedClass()
    var deo2 = self.DerivedExtUnionX(24) // expected-error {{type 'ThisDerived1' has no member 'DerivedExtUnionX'}}
    var deo3 = self.DerivedExtNestedUnion.DerivedExtUnionX(24)
    var det2 = self.DerivedExtNestedTypealias()

    self.Type // expected-error {{type 'ThisDerived1' has no member 'Type'}}
  }

  // FIXME: Partial application diagnostic is applied incorrectly for some
  // test cases (https://github.com/apple/swift/issues/57572).
  class func staticTestSuper1() {
    super.baseInstanceVar = 42 // expected-error {{member 'baseInstanceVar' of type 'ThisBase1' cannot be used in static context}}
    super.baseProp = 42 // expected-error {{member 'baseProp' of type 'ThisBase1' cannot be used in static context}}
    super.baseFunc0() // expected-error {{instance member 'baseFunc0' of type 'ThisBase1' cannot be used in static context}}
    // expected-error@-1 {{cannot reference 'super' instance method with metatype base as function value}}
    super.baseFunc0(ThisBase1())() // expected-error {{cannot reference 'super' instance method with metatype base as function value}}
    super.baseFunc1(42) // expected-error {{instance member 'baseFunc1' of type 'ThisBase1' cannot be used in static context}}
    // expected-error@-1 {{cannot reference 'super' instance method with metatype base as function value}}
    super.baseFunc1(ThisBase1())(42) // expected-error {{cannot reference 'super' instance method with metatype base as function value}}
    super[0] = 42.0 // expected-error {{instance member 'subscript' of type 'ThisBase1' cannot be used in static context}}
    super.baseStaticVar = 42
    super.baseStaticProp = 42
    super.baseStaticFunc0()

    super.baseExtProp = 42 // expected-error {{member 'baseExtProp' of type 'ThisBase1' cannot be used in static context}}
    super.baseExtFunc0() // expected-error {{instance member 'baseExtFunc0' of type 'ThisBase1' cannot be used in static context}}
    // expected-error@-1 {{cannot reference 'super' instance method with metatype base as function value}}
    super.baseExtStaticVar = 42 // expected-error {{instance member 'baseExtStaticVar' of type 'ThisBase1' cannot be used in static context}}
    super.baseExtStaticProp = 42 // expected-error {{member 'baseExtStaticProp' of type 'ThisBase1' cannot be used in static context}}
    super.baseExtStaticFunc0()

    var bs2 = super.BaseNestedStruct()
    var bc2 = super.BaseNestedClass()
    var bo2 = super.BaseUnionX(24) // expected-error {{type 'ThisBase1' has no member 'BaseUnionX'}}
    var bo3 = super.BaseNestedUnion.BaseUnionX(24)
    var bt2 = super.BaseNestedTypealias()

    super.derivedInstanceVar = 42 // expected-error {{type 'ThisBase1' has no member 'derivedInstanceVar'}}
    super.derivedProp = 42 // expected-error {{type 'ThisBase1' has no member 'derivedProp'}}
    super.derivedFunc0() // expected-error {{type 'ThisBase1' has no member 'derivedFunc0'}}
    super.derivedStaticVar = 42 // expected-error {{type 'ThisBase1' has no member 'derivedStaticVar'}}
    super.derivedStaticProp = 42 // expected-error {{type 'ThisBase1' has no member 'derivedStaticProp'}}
    super.derivedStaticFunc0() // expected-error {{type 'ThisBase1' has no member 'derivedStaticFunc0'}}

    super.derivedExtProp = 42 // expected-error {{type 'ThisBase1' has no member 'derivedExtProp'}}
    super.derivedExtFunc0() // expected-error {{type 'ThisBase1' has no member 'derivedExtFunc0'}}
    super.derivedExtStaticVar = 42 // expected-error {{type 'ThisBase1' has no member 'derivedExtStaticVar'; did you mean 'baseExtStaticVar'?}}
    super.derivedExtStaticProp = 42 // expected-error {{type 'ThisBase1' has no member 'derivedExtStaticProp'; did you mean 'baseExtStaticProp'?}}
    super.derivedExtStaticFunc0() // expected-error {{type 'ThisBase1' has no member 'derivedExtStaticFunc0'; did you mean 'baseExtStaticFunc0'?}}

    var ds2 = super.DerivedNestedStruct() // expected-error {{type 'ThisBase1' has no member 'DerivedNestedStruct'}}
    var dc2 = super.DerivedNestedClass() // expected-error {{type 'ThisBase1' has no member 'DerivedNestedClass'}}
    var do2 = super.DerivedUnionX(24) // expected-error {{type 'ThisBase1' has no member 'DerivedUnionX'}}
    var do3 = super.DerivedNestedUnion.DerivedUnionX(24) // expected-error {{type 'ThisBase1' has no member 'DerivedNestedUnion'}}
    var dt2 = super.DerivedNestedTypealias(42) // expected-error {{type 'ThisBase1' has no member 'DerivedNestedTypealias'}}

    var des2 = super.DerivedExtNestedStruct() // expected-error {{type 'ThisBase1' has no member 'DerivedExtNestedStruct'; did you mean 'BaseExtNestedStruct'?}}
    var dec2 = super.DerivedExtNestedClass() // expected-error {{type 'ThisBase1' has no member 'DerivedExtNestedClass'; did you mean 'BaseExtNestedClass'?}}
    var deo2 = super.DerivedExtUnionX(24) // expected-error {{type 'ThisBase1' has no member 'DerivedExtUnionX'}}
    var deo3 = super.DerivedExtNestedUnion.DerivedExtUnionX(24) // expected-error {{type 'ThisBase1' has no member 'DerivedExtNestedUnion'; did you mean 'BaseExtNestedUnion'?}}
    var det2 = super.DerivedExtNestedTypealias(42) // expected-error {{type 'ThisBase1' has no member 'DerivedExtNestedTypealias'; did you mean 'BaseExtNestedTypealias'?}}

    super.Type // expected-error {{type 'ThisBase1' has no member 'Type'}}
  }
}

protocol Crawlable {}
extension Crawlable {
   static func crawl() {}
}
struct GenericChameleon<U>: Crawlable {
  static func chameleon() {}

  func testStaticOnInstance(arg: GenericChameleon<Never>) {
    arg.chameleon() // expected-error {{static member 'chameleon' cannot be used on instance of type 'GenericChameleon<Never>'}} {{5-8=GenericChameleon<Never>}}
    arg.crawl() // expected-error {{static member 'crawl' cannot be used on instance of type 'GenericChameleon<Never>'}} {{5-8=GenericChameleon<Never>}}
  }
}

extension ThisBase1 {
  var baseExtProp : Int {
    get {
      return 42
    }
    set {}
  }

  func baseExtFunc0() {}

  var baseExtStaticVar: Int // expected-error {{extensions must not contain stored properties}} // expected-note 2 {{'baseExtStaticVar' declared here}}

  var baseExtStaticProp: Int { // expected-note 2 {{'baseExtStaticProp' declared here}}
    get {
      return 42
    }
    set {}
  }

  class func baseExtStaticFunc0() {} // expected-note {{'baseExtStaticFunc0' declared here}}

  struct BaseExtNestedStruct {} // expected-note {{did you mean 'BaseExtNestedStruct'?}} // expected-note {{'BaseExtNestedStruct' declared here}}
  class BaseExtNestedClass { // expected-note {{'BaseExtNestedClass' declared here}}
    init() { }
  }
  enum BaseExtNestedUnion { // expected-note {{'BaseExtNestedUnion' declared here}}
    case BaseExtUnionX(Int)
  }

  typealias BaseExtNestedTypealias = Int // expected-note {{did you mean 'BaseExtNestedTypealias'?}} // expected-note {{'BaseExtNestedTypealias' declared here}}
}

extension ThisDerived1 {
  var derivedExtProp : Int {
    get {
      return 42
    }
    set {}
  }

  func derivedExtFunc0() {}

  var derivedExtStaticVar: Int // expected-error {{extensions must not contain stored properties}}

  var derivedExtStaticProp: Int {
    get {
      return 42
    }
    set {}
  }

  class func derivedExtStaticFunc0() {}

  struct DerivedExtNestedStruct {}
  class DerivedExtNestedClass {
    init() { }
  }
  enum DerivedExtNestedUnion { // expected-note {{did you mean 'DerivedExtNestedUnion'?}}
    case DerivedExtUnionX(Int)
  }

  typealias DerivedExtNestedTypealias = Int
}

// <rdar://problem/11554141>
func shadowbug() {
  let Foo = 10
  func g() {
    struct S {
      var x : Foo
      typealias Foo = Int
    }
  }
  _ = Foo
}
func scopebug() {
  let Foo = 10
  struct S {
    typealias Foo = Int
  }
  _ = Foo
}
struct Ordering {
  var x : Foo
  typealias Foo = Int
}

// <rdar://problem/12202655>
class Outer {
  class Inner {}
  class MoreInner : Inner {}
}

func makeGenericStruct<S>(_ x: S) -> GenericStruct<S> {
  return GenericStruct<S>()
}
struct GenericStruct<T> {}


// <rdar://problem/13952064>
extension Outer {
  class ExtInner {}
}

// <rdar://problem/14149537>
func useProto<R : MyProto>(_ value: R) -> R.Element {
  return value.get()
}

protocol MyProto {
  associatedtype Element
  func get() -> Element
}


// <rdar://problem/14488311>
struct DefaultArgumentFromExtension {
  func g(_ x: @escaping (DefaultArgumentFromExtension) -> () -> () = f) {
    let f = 42
    var x2 = x
    x2 = f // expected-error{{cannot assign value of type 'Int' to type '(DefaultArgumentFromExtension) -> () -> ()'}}
    _ = x2
  }
  var x : (DefaultArgumentFromExtension) -> () -> () = f
}
extension DefaultArgumentFromExtension {
  func f() {}
}

struct MyStruct {
  var state : Bool
  init() { state = true }
  mutating func mod() {state = false}
  // expected-note @+1 {{mark method 'mutating' to make 'self' mutable}} {{3-3=mutating }}
  func foo() { mod() } // expected-error {{cannot use mutating member on immutable value: 'self' is immutable}}
}


// <rdar://problem/19935319> QoI: poor diagnostic initializing a variable with a non-class func
class Test19935319 {
  let i = getFoo()  // expected-error {{cannot use instance member 'getFoo' within property initializer; property initializers run before 'self' is available}}

  func getFoo() -> Int {}
}

class Test19935319G<T> {
  let i = getFoo()
  // expected-error@-1 {{cannot use instance member 'getFoo' within property initializer; property initializers run before 'self' is available}}
  func getFoo() -> Int { return 42 }
}

// <rdar://problem/27013358> Crash using instance member as default parameter
class rdar27013358 {
  let defaultValue = 1
  func returnTwo() -> Int {
    return 2
  }
  init(defaulted value: Int = defaultValue) {} // expected-error {{cannot use instance member 'defaultValue' as a default parameter}}
  init(another value: Int = returnTwo()) {} // expected-error {{cannot use instance member 'returnTwo' as a default parameter}}
}

class rdar27013358G<T> {
  let defaultValue = 1
  func returnTwo() -> Int {
    return 2
  }
  init(defaulted value: Int = defaultValue) {} // expected-error {{cannot use instance member 'defaultValue' as a default parameter}}
  init(another value: Int = returnTwo()) {} // expected-error {{cannot use instance member 'returnTwo' as a default parameter}}
}

// <rdar://problem/23904262> QoI: ivar default initializer cannot reference other default initialized ivars?
class r23904262 {
  let x = 1
  let y = x // expected-error {{cannot use instance member 'x' within property initializer; property initializers run before 'self' is available}}
}


// <rdar://problem/21677702> Static method reference in static var doesn't work
class r21677702 {
  static func method(value: Int) -> Int { return value }
  static let x = method(value: 123)
  static let y = method(123) // expected-error {{missing argument label 'value:' in call}}
}


// <rdar://problem/31762378> lazy properties don't have to use "self." in their initializers.
class r16954496 {
  func bar() {}
  lazy var x: Array<() -> Void> = [bar]
}



// <rdar://problem/27413116> [Swift] Using static constant defined in enum when in switch statement doesn't compile
enum MyEnum {
  case one
  case two
  case oneTwoThree

  static let kMyConstant = "myConstant"
}

switch "someString" {
case MyEnum.kMyConstant: // this causes a compiler error
  print("yay")
case MyEnum.self.kMyConstant: // this works fine
  print("hmm")
default:
  break
}

func foo() {
  _ = MyEnum.One // expected-error {{enum type 'MyEnum' has no case 'One'; did you mean 'one'?}}{{14-17=one}}
  _ = MyEnum.Two // expected-error {{enum type 'MyEnum' has no case 'Two'; did you mean 'two'?}}{{14-17=two}}
  _ = MyEnum.OneTwoThree // expected-error {{enum type 'MyEnum' has no case 'OneTwoThree'; did you mean 'oneTwoThree'?}}{{14-25=oneTwoThree}}
}

enum MyGenericEnum<T> {
  case one(T)
  case oneTwo(T)
}

func foo1() {
  _ = MyGenericEnum<Int>.One // expected-error {{enum type 'MyGenericEnum<Int>' has no case 'One'; did you mean 'one'?}}{{26-29=one}}
  _ = MyGenericEnum<Int>.OneTwo // expected-error {{enum type 'MyGenericEnum<Int>' has no case 'OneTwo'; did you mean 'oneTwo'?}}{{26-32=oneTwo}}
}

// https://github.com/apple/swift/issues/46665
do {
  let x = 5
  if x < 0, let x = Optional(1) { } // expected-warning {{immutable value 'x' was never used; consider replacing with '_' or removing it}}
}

struct Person {
  let name: String?
}

struct Company { // expected-note 2{{'Company' declared here}}
  let owner: Person?
}

func test1() {
  let example: Company? = Company(owner: Person(name: "Owner"))
  if let person = aCompany.owner, // expected-error {{cannot find 'aCompany' in scope; did you mean 'Company'?}}
     let aCompany = example {
    _ = person
  }
}

func test2() {
  let example: Company? = Company(owner: Person(name: "Owner"))
  guard let person = aCompany.owner, // expected-error {{cannot find 'aCompany' in scope; did you mean 'Company'?}}
        let aCompany = example else { return }
}

func test3() {
  var c: String? = "c" // expected-note {{'c' declared here}}
  if let a = b = c, let b = c { // expected-error {{cannot find 'b' in scope; did you mean 'c'?}}
    _ = b
  }
}

// rdar://problem/22587551
class ShadowingGenericParameter<T> {
  typealias T = Int
  func foo (t : T) {}
}

ShadowingGenericParameter<String>().foo(t: "hi")

// rdar://problem/51266778
struct PatternBindingWithTwoVars1 { var x = 3, y = x }
// expected-error@-1 {{cannot use instance member 'x' within property initializer; property initializers run before 'self' is available}}

struct PatternBindingWithTwoVars2 { var x = y, y = 3 }
// expected-error@-1 {{cannot use instance member 'y' within property initializer; property initializers run before 'self' is available}}

struct PatternBindingWithTwoVars3 { var x = y, y = x }
// expected-error@-1 {{circular reference}}
// expected-note@-2 {{through reference here}}
// expected-note@-3 {{through reference here}}
// expected-note@-4 {{through reference here}}
// expected-note@-5 {{through reference here}}
// expected-note@-6 {{through reference here}}

// https://github.com/apple/swift/issues/51518
do {
  let closure1 = { closure2() } // expected-error {{circular reference}} expected-note {{through reference here}} expected-note {{through reference here}}
  let closure2 = { closure1() } // expected-note {{through reference here}} expected-note {{through reference here}} expected-note {{through reference here}}
}

func color(with value: Int) -> Int {
  return value
}

func useColor() {
  let color = color(with: 123)
  _ = color
}
