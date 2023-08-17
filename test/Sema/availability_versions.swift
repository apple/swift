// RUN: %target-typecheck-verify-swift -target %target-cpu-apple-macosx10.50 -disable-objc-attr-requires-foundation-module
// RUN: not %target-swift-frontend -target %target-cpu-apple-macosx10.50 -disable-objc-attr-requires-foundation-module -typecheck %s 2>&1 | %FileCheck %s '--implicit-check-not=<unknown>:0'

// Make sure we do not emit availability errors or warnings when -disable-availability-checking is passed
// RUN: not %target-swift-frontend -target %target-cpu-apple-macosx10.50 -typecheck -disable-objc-attr-requires-foundation-module -disable-availability-checking %s 2>&1 | %FileCheck %s '--implicit-check-not=error:' '--implicit-check-not=warning:'

// REQUIRES: OS=macosx

func markUsed<T>(_ t: T) {}

@available(OSX, introduced: 10.9)
func globalFuncAvailableOn10_9() -> Int { return 9 }

@available(OSX, introduced: 10.51)
func globalFuncAvailableOn10_51() -> Int { return 10 }

@available(OSX, introduced: 10.52)
func globalFuncAvailableOn10_52() -> Int { return 11 }

// Top level should reflect the minimum deployment target.
let ignored1: Int = globalFuncAvailableOn10_9()

let ignored2: Int = globalFuncAvailableOn10_51() // expected-error {{'globalFuncAvailableOn10_51()' is only available in macOS 10.51 or newer}}
    // expected-note@-1 {{add 'if #available' version check}}

let ignored3: Int = globalFuncAvailableOn10_52() // expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
    // expected-note@-1 {{add 'if #available' version check}}

// Functions without annotations should reflect the minimum deployment target.
func functionWithoutAvailability() {
      // expected-note@-1 2{{add @available attribute to enclosing global function}}

  let _: Int = globalFuncAvailableOn10_9()

  let _: Int = globalFuncAvailableOn10_51() // expected-error {{'globalFuncAvailableOn10_51()' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  let _: Int = globalFuncAvailableOn10_52() // expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

// Functions with annotations should refine their bodies.
@available(OSX, introduced: 10.51)
func functionAvailableOn10_51() {
  let _: Int = globalFuncAvailableOn10_9()
  let _: Int = globalFuncAvailableOn10_51()

  // Nested functions should get their own refinement context.
  @available(OSX, introduced: 10.52)
  func innerFunctionAvailableOn10_52() {
    let _: Int = globalFuncAvailableOn10_9()
    let _: Int = globalFuncAvailableOn10_51()
    let _: Int = globalFuncAvailableOn10_52()
  }

  let _: Int = globalFuncAvailableOn10_52() // expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

// Still allow other availability annotations on script-mode globals
@available(OSX, deprecated: 10.51)
var deprecatedGlobalInScriptMode: Int = 5

if #available(OSX 10.51, *) {
  let _: Int = globalFuncAvailableOn10_51()
  let _: Int = globalFuncAvailableOn10_52() // expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

if #available(OSX 10.51, *) {
  let _: Int = globalFuncAvailableOn10_51()
  let _: Int = globalFuncAvailableOn10_52() // expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
} else {
  let _: Int = globalFuncAvailableOn10_9()
  let _: Int = globalFuncAvailableOn10_51() // expected-error {{'globalFuncAvailableOn10_51()' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

@available(OSX, introduced: 10.51)
@available(iOS, introduced: 8.0)
func globalFuncAvailableOnOSX10_51AndiOS8_0() -> Int { return 10 }

if #available(OSX 10.51, iOS 8.0, *) {
  let _: Int = globalFuncAvailableOnOSX10_51AndiOS8_0()
}

if #available(iOS 9.0, *) {
  let _: Int = globalFuncAvailableOnOSX10_51AndiOS8_0() // expected-error {{'globalFuncAvailableOnOSX10_51AndiOS8_0()' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

// Multiple potentially unavailable references in a single statement

let ignored4: (Int, Int) = (globalFuncAvailableOn10_51(), globalFuncAvailableOn10_52()) // expected-error {{'globalFuncAvailableOn10_51()' is only available in macOS 10.51 or newer}}  expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
    // expected-note@-1 2{{add 'if #available' version check}}


_ = globalFuncAvailableOn10_9()

let ignored5 = globalFuncAvailableOn10_51 // expected-error {{'globalFuncAvailableOn10_51()' is only available in macOS 10.51 or newer}}
    // expected-note@-1 {{add 'if #available' version check}}

_ = globalFuncAvailableOn10_51() // expected-error {{'globalFuncAvailableOn10_51()' is only available in macOS 10.51 or newer}}
    // expected-note@-1 {{add 'if #available' version check}}

// Overloaded global functions
@available(OSX, introduced: 10.9)
func overloadedFunction() {}

@available(OSX, introduced: 10.51)
func overloadedFunction(_ on1010: Int) {}

overloadedFunction()
overloadedFunction(0) // expected-error {{'overloadedFunction' is only available in macOS 10.51 or newer}}
    // expected-note@-1 {{add 'if #available' version check}}

// Potentially unavailable methods

class ClassWithPotentiallyUnavailableMethod {
    // expected-note@-1 {{add @available attribute to enclosing class}}

  @available(OSX, introduced: 10.9)
  func methAvailableOn10_9() {}
  
  @available(OSX, introduced: 10.51)
  func methAvailableOn10_51() {}
  
  @available(OSX, introduced: 10.51)
  class func classMethAvailableOn10_51() {}
  
  func someOtherMethod() {
    // expected-note@-1 {{add @available attribute to enclosing instance method}}

    methAvailableOn10_9()
    methAvailableOn10_51() // expected-error {{'methAvailableOn10_51()' is only available in macOS 10.51 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}
  }
}

func callPotentiallyUnavailableMethods(_ o: ClassWithPotentiallyUnavailableMethod) {
      // expected-note@-1 2{{add @available attribute to enclosing global function}}

  let m10_9 = o.methAvailableOn10_9
  m10_9()
  
  let m10_51 = o.methAvailableOn10_51 // expected-error {{'methAvailableOn10_51()' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  m10_51()
  
  o.methAvailableOn10_9()
  o.methAvailableOn10_51() // expected-error {{'methAvailableOn10_51()' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

func callPotentiallyUnavailableMethodsViaIUO(_ o: ClassWithPotentiallyUnavailableMethod!) {
      // expected-note@-1 2{{add @available attribute to enclosing global function}}

  let m10_9 = o.methAvailableOn10_9
  m10_9()
  
  let m10_51 = o.methAvailableOn10_51 // expected-error {{'methAvailableOn10_51()' is only available in macOS 10.51 or newer}}
      
      // expected-note@-2 {{add 'if #available' version check}}

  m10_51()
  
  o.methAvailableOn10_9()
  o.methAvailableOn10_51() // expected-error {{'methAvailableOn10_51()' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

func callPotentiallyUnavailableClassMethod() {
      // expected-note@-1 2{{add @available attribute to enclosing global function}}

  ClassWithPotentiallyUnavailableMethod.classMethAvailableOn10_51() // expected-error {{'classMethAvailableOn10_51()' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
  
  let m10_51 = ClassWithPotentiallyUnavailableMethod.classMethAvailableOn10_51 // expected-error {{'classMethAvailableOn10_51()' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  m10_51()
}

class SubClassWithPotentiallyUnavailableMethod : ClassWithPotentiallyUnavailableMethod {
        // expected-note@-1 {{add @available attribute to enclosing class}}
  func someMethod() {
        // expected-note@-1 {{add @available attribute to enclosing instance method}}

    methAvailableOn10_9()
    methAvailableOn10_51() // expected-error {{'methAvailableOn10_51()' is only available in macOS 10.51 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}
  }
}

class SubClassOverridingPotentiallyUnavailableMethod : ClassWithPotentiallyUnavailableMethod {
        // expected-note@-1 2{{add @available attribute to enclosing class}}

  override func methAvailableOn10_51() {
        // expected-note@-1 2{{add @available attribute to enclosing instance method}}
    methAvailableOn10_9()
    super.methAvailableOn10_51() // expected-error {{'methAvailableOn10_51()' is only available in macOS 10.51 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}
    
    let m10_9 = super.methAvailableOn10_9
    m10_9()
    
    let m10_51 = super.methAvailableOn10_51 // expected-error {{'methAvailableOn10_51()' is only available in macOS 10.51 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}
    m10_51()
  }
  
  func someMethod() {
    methAvailableOn10_9()
    // Calling our override should be fine
    methAvailableOn10_51()
  }
}

class ClassWithPotentiallyUnavailableOverloadedMethod {
  @available(OSX, introduced: 10.9)
  func overloadedMethod() {}

  @available(OSX, introduced: 10.51)
  func overloadedMethod(_ on1010: Int) {}
}

func callPotentiallyUnavailableOverloadedMethod(_ o: ClassWithPotentiallyUnavailableOverloadedMethod) {
      // expected-note@-1 {{add @available attribute to enclosing global function}}

  o.overloadedMethod()
  o.overloadedMethod(0) // expected-error {{'overloadedMethod' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

// Initializers

class ClassWithPotentiallyUnavailableInitializer {
    // expected-note@-1 {{add @available attribute to enclosing class}}

  @available(OSX, introduced: 10.9)
  required init() {  }
  
  @available(OSX, introduced: 10.51)
  required init(_ val: Int) {  }
  
  convenience init(s: String) {
        // expected-note@-1 {{add @available attribute to enclosing initializer}}
    
    self.init(5) // expected-error {{'init(_:)' is only available in macOS 10.51 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}
  }
  
  @available(OSX, introduced: 10.51)
  convenience init(onlyOn1010: String) {
    self.init(5)
  }
}

func callPotentiallyUnavailableInitializer() {
      // expected-note@-1 2{{add @available attribute to enclosing global function}}

  _ = ClassWithPotentiallyUnavailableInitializer()
  _ = ClassWithPotentiallyUnavailableInitializer(5) // expected-error {{'init(_:)' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
  
  let i = ClassWithPotentiallyUnavailableInitializer.self
  _ = i.init()
  _ = i.init(5) // expected-error {{'init(_:)' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

class SuperWithWithPotentiallyUnavailableInitializer {
  @available(OSX, introduced: 10.9)
  init() {  }
  
  @available(OSX, introduced: 10.51)
  init(_ val: Int) {  }
}

class SubOfClassWithPotentiallyUnavailableInitializer : SuperWithWithPotentiallyUnavailableInitializer {
    // expected-note@-1 {{add @available attribute to enclosing class}}

  override init(_ val: Int) {
        // expected-note@-1 {{add @available attribute to enclosing initializer}}

    super.init(5) // expected-error {{'init(_:)' is only available in macOS 10.51 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}
  }
  
  override init() {
    super.init()
  }
  
  @available(OSX, introduced: 10.51)
  init(on1010: Int) {
    super.init(22)
  }
}

// Properties

class ClassWithPotentiallyUnavailableProperties {
    // expected-note@-1 4{{add @available attribute to enclosing class}}

  var nonLazyAvailableOn10_9Stored: Int = 9

  @available(OSX, introduced: 10.51) // expected-error {{stored properties cannot be marked potentially unavailable with '@available'}}
  var nonLazyAvailableOn10_51Stored : Int = 10

  @available(OSX, introduced: 10.51) // expected-error {{stored properties cannot be marked potentially unavailable with '@available'}}
  let nonLazyLetAvailableOn10_51Stored : Int = 10

  // Make sure that we don't emit a Fix-It to mark a stored property as potentially unavailable.
  // We don't support potentially unavailable stored properties yet.
  var storedPropertyOfPotentiallyUnavailableType: ClassAvailableOn10_51? = nil // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}

  @available(OSX, introduced: 10.9)
  lazy var availableOn10_9Stored: Int = 9
  
  @available(OSX, introduced: 10.51) // expected-error {{stored properties cannot be marked potentially unavailable with '@available'}}
  lazy var availableOn10_51Stored : Int = 10

  @available(OSX, introduced: 10.9)
  var availableOn10_9Computed: Int {
    get {
      let _: Int = availableOn10_51Stored // expected-error {{'availableOn10_51Stored' is only available in macOS 10.51 or newer}}
          // expected-note@-1 {{add 'if #available' version check}}
      
      if #available(OSX 10.51, *) {
        let _: Int = availableOn10_51Stored
      }
      
      return availableOn10_9Stored
    }
    set(newVal) {
      availableOn10_9Stored = newVal
    }
  }
  
  @available(OSX, introduced: 10.51)
  var availableOn10_51Computed: Int {
    get {
      return availableOn10_51Stored
    }
    set(newVal) {
      availableOn10_51Stored = newVal
    }
  }
  
  var propWithSetterOnlyAvailableOn10_51 : Int {
      // expected-note@-1 {{add @available attribute to enclosing property}}
    get {
      _ = globalFuncAvailableOn10_51() // expected-error {{'globalFuncAvailableOn10_51()' is only available in macOS 10.51 or newer}}
          // expected-note@-1 {{add 'if #available' version check}}
      return 0
    }
    @available(OSX, introduced: 10.51)
    set(newVal) {
    _ = globalFuncAvailableOn10_51()
    }
  }
  
  var propWithGetterOnlyAvailableOn10_51 : Int {
      // expected-note@-1 {{add @available attribute to enclosing property}}
    @available(OSX, introduced: 10.51)
    get {
      _ = globalFuncAvailableOn10_51()
      return 0
    }
    set(newVal) {
      _ = globalFuncAvailableOn10_51() // expected-error {{'globalFuncAvailableOn10_51()' is only available in macOS 10.51 or newer}}
          // expected-note@-1 {{add 'if #available' version check}}
    }
  }
  
  var propWithGetterAndSetterOnlyAvailableOn10_51 : Int {
    @available(OSX, introduced: 10.51)
    get {
      return 0
    }
    @available(OSX, introduced: 10.51)
    set(newVal) {
    }
  }
  
  var propWithSetterOnlyAvailableOn10_51ForNestedMemberRef : ClassWithPotentiallyUnavailableProperties {
    get {
      return ClassWithPotentiallyUnavailableProperties()
    }
    @available(OSX, introduced: 10.51)
    set(newVal) {
    }
  }
  
  var propWithGetterOnlyAvailableOn10_51ForNestedMemberRef : ClassWithPotentiallyUnavailableProperties {
    @available(OSX, introduced: 10.51)
    get {
      return ClassWithPotentiallyUnavailableProperties()
    }
    set(newVal) {
    }
  }
}

@available(OSX, introduced: 10.51)
class ClassWithReferencesInInitializers {
  var propWithInitializer10_51: Int = globalFuncAvailableOn10_51()

  var propWithInitializer10_52: Int = globalFuncAvailableOn10_52() // expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}

  lazy var lazyPropWithInitializer10_51: Int = globalFuncAvailableOn10_51()

  lazy var lazyPropWithInitializer10_52: Int = globalFuncAvailableOn10_52() // expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
}

func accessPotentiallyUnavailableProperties(_ o: ClassWithPotentiallyUnavailableProperties) {
      // expected-note@-1 17{{add @available attribute to enclosing global function}}
  // Stored properties
  let _: Int = o.availableOn10_9Stored
  let _: Int = o.availableOn10_51Stored // expected-error {{'availableOn10_51Stored' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
  
  o.availableOn10_9Stored = 9
  o.availableOn10_51Stored = 10 // expected-error {{'availableOn10_51Stored' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  // Computed Properties
  let _: Int = o.availableOn10_9Computed
  let _: Int = o.availableOn10_51Computed // expected-error {{'availableOn10_51Computed' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
  
  o.availableOn10_9Computed = 9
  o.availableOn10_51Computed = 10 // expected-error {{'availableOn10_51Computed' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
  
  // Getter allowed on 10.9 but setter is not
  let _: Int = o.propWithSetterOnlyAvailableOn10_51
  o.propWithSetterOnlyAvailableOn10_51 = 5 // expected-error {{setter for 'propWithSetterOnlyAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
  
  if #available(OSX 10.51, *) {
    // Setter is allowed on 10.51 and greater
    o.propWithSetterOnlyAvailableOn10_51 = 5
  }
  
  // Setter allowed on 10.9 but getter is not
  o.propWithGetterOnlyAvailableOn10_51 = 5
  let _: Int = o.propWithGetterOnlyAvailableOn10_51 // expected-error {{getter for 'propWithGetterOnlyAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  if #available(OSX 10.51, *) {
    // Getter is allowed on 10.51 and greater
    let _: Int = o.propWithGetterOnlyAvailableOn10_51
  }
  
  // Tests for nested member refs
  
  // Both getters are potentially unavailable.
  let _: Int = o.propWithGetterOnlyAvailableOn10_51ForNestedMemberRef.propWithGetterOnlyAvailableOn10_51 // expected-error {{getter for 'propWithGetterOnlyAvailableOn10_51ForNestedMemberRef' is only available in macOS 10.51 or newer}} expected-error {{getter for 'propWithGetterOnlyAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 2{{add 'if #available' version check}}

  // Nested getter is potentially unavailable, outer getter is available
  let _: Int = o.propWithGetterOnlyAvailableOn10_51ForNestedMemberRef.propWithSetterOnlyAvailableOn10_51 // expected-error {{getter for 'propWithGetterOnlyAvailableOn10_51ForNestedMemberRef' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  // Nested getter is available, outer getter is potentially unavailable
  let _:Int = o.propWithSetterOnlyAvailableOn10_51ForNestedMemberRef.propWithGetterOnlyAvailableOn10_51 // expected-error {{getter for 'propWithGetterOnlyAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  // Both getters are always available.
  let _: Int = o.propWithSetterOnlyAvailableOn10_51ForNestedMemberRef.propWithSetterOnlyAvailableOn10_51
  
  
  // Nesting in source of assignment
  var v: Int

  v = o.propWithGetterOnlyAvailableOn10_51 // expected-error {{getter for 'propWithGetterOnlyAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  v = (o.propWithGetterOnlyAvailableOn10_51) // expected-error {{getter for 'propWithGetterOnlyAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  _ = v // muffle warning

  // Inout requires access to both getter and setter
  
  func takesInout(_ i : inout Int) { }
  
  takesInout(&o.propWithGetterOnlyAvailableOn10_51) // expected-error {{cannot pass as inout because getter for 'propWithGetterOnlyAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  takesInout(&o.propWithSetterOnlyAvailableOn10_51) // expected-error {{cannot pass as inout because setter for 'propWithSetterOnlyAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
  
  takesInout(&o.propWithGetterAndSetterOnlyAvailableOn10_51) // expected-error {{cannot pass as inout because getter for 'propWithGetterAndSetterOnlyAvailableOn10_51' is only available in macOS 10.51 or newer}} expected-error {{cannot pass as inout because setter for 'propWithGetterAndSetterOnlyAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 2{{add 'if #available' version check}}

  takesInout(&o.availableOn10_9Computed)
  takesInout(&o.propWithGetterOnlyAvailableOn10_51ForNestedMemberRef.availableOn10_9Computed) // expected-error {{getter for 'propWithGetterOnlyAvailableOn10_51ForNestedMemberRef' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

// _silgen_name

@_silgen_name("SomeName")
@available(OSX, introduced: 10.51)
func funcWith_silgen_nameAvailableOn10_51(_ p: ClassAvailableOn10_51?) -> ClassAvailableOn10_51

// Enums

@available(OSX, introduced: 10.51)
enum EnumIntroducedOn10_51 {
 case Element
}

@available(OSX, introduced: 10.52)
enum EnumIntroducedOn10_52 {
 case Element
}

@available(OSX, introduced: 10.51)
enum CompassPoint {
  case North
  case South
  case East

  @available(OSX, introduced: 10.52)
  case West

  case WithAvailableByEnumPayload(p : EnumIntroducedOn10_51)

  // expected-error@+1 {{enum cases with associated values cannot be marked potentially unavailable with '@available'}}
  @available(OSX, introduced: 10.52)
  case WithAvailableByEnumElementPayload(p : EnumIntroducedOn10_52)

  // expected-error@+1 2{{enum cases with associated values cannot be marked potentially unavailable with '@available'}}
  @available(OSX, introduced: 10.52)
  case WithAvailableByEnumElementPayload1(p : EnumIntroducedOn10_52), WithAvailableByEnumElementPayload2(p : EnumIntroducedOn10_52)

  case WithPotentiallyUnavailablePayload(p : EnumIntroducedOn10_52) // expected-error {{'EnumIntroducedOn10_52' is only available in macOS 10.52 or newer}}

  case WithPotentiallyUnavailablePayload1(p : EnumIntroducedOn10_52), WithPotentiallyUnavailablePayload2(p : EnumIntroducedOn10_52) // expected-error 2{{'EnumIntroducedOn10_52' is only available in macOS 10.52 or newer}}
  
  @available(OSX, unavailable)
  case WithPotentiallyUnavailablePayload3(p : EnumIntroducedOn10_52)
}

@available(OSX, introduced: 10.52)
func functionTakingEnumIntroducedOn10_52(_ e: EnumIntroducedOn10_52) { }

func useEnums() {
      // expected-note@-1 3{{add @available attribute to enclosing global function}}
  let _: CompassPoint = .North // expected-error {{'CompassPoint' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  if #available(OSX 10.51, *) {
    let _: CompassPoint = .North

    let _: CompassPoint = .West // expected-error {{'West' is only available in macOS 10.52 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}
  }

  if #available(OSX 10.52, *) {
    let _: CompassPoint = .West
  }

  // Pattern matching on an enum element does not require it to be definitely available
  if #available(OSX 10.51, *) {
    let point: CompassPoint = .North
    switch (point) {
      case .North, .South, .East:
        markUsed("NSE")
      case .West: // We do not expect an error here
        markUsed("W")

      case .WithPotentiallyUnavailablePayload(_):
        markUsed("WithPotentiallyUnavailablePayload")
      case .WithPotentiallyUnavailablePayload1(_):
        markUsed("WithPotentiallyUnavailablePayload1")
      case .WithPotentiallyUnavailablePayload2(_):
        markUsed("WithPotentiallyUnavailablePayload2")

      case .WithAvailableByEnumPayload(_):
        markUsed("WithAvailableByEnumPayload")
      case .WithAvailableByEnumElementPayload1(_):
        markUsed("WithAvailableByEnumElementPayload1")
      case .WithAvailableByEnumElementPayload2(_):
        markUsed("WithAvailableByEnumElementPayload2")
      case .WithAvailableByEnumElementPayload(let p):
        markUsed("WithAvailableByEnumElementPayload")

        // For the moment, we do not incorporate enum element availability into 
        // TRC construction. Perhaps we should?
        functionTakingEnumIntroducedOn10_52(p)  // expected-error {{'functionTakingEnumIntroducedOn10_52' is only available in macOS 10.52 or newer}}
          
          // expected-note@-2 {{add 'if #available' version check}}
    }
  }
}

// Classes

@available(OSX, introduced: 10.9)
class ClassAvailableOn10_9 {
  func someMethod() {}
  class func someClassMethod() {}
  var someProp : Int = 22
}

@available(OSX, introduced: 10.51)
class ClassAvailableOn10_51 { // expected-note {{enclosing scope requires availability of macOS 10.51 or newer}}
  func someMethod() {}
  class func someClassMethod() {
    let _ = ClassAvailableOn10_51()
  }
  var someProp : Int = 22

  @available(OSX, introduced: 10.9) // expected-error {{instance method cannot be more available than enclosing scope}}
  func someMethodAvailableOn10_9() { }

  @available(OSX, unavailable)
  func someMethodUnavailable() { }

  @available(*, unavailable)
  func someMethodUniversallyUnavailable() { }

  @available(OSX, introduced: 10.52)
  var propWithGetter: Int { // expected-note{{enclosing scope requires availability of macOS 10.52 or newer}}
    @available(OSX, introduced: 10.51) // expected-error {{getter cannot be more available than enclosing scope}}
    get { return 0 }
  }
}

func classAvailability() {
      // expected-note@-1 3{{add @available attribute to enclosing global function}}
  ClassAvailableOn10_9.someClassMethod()
  ClassAvailableOn10_51.someClassMethod() // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  _ = ClassAvailableOn10_9.self
  _ = ClassAvailableOn10_51.self // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
  
  let o10_9 = ClassAvailableOn10_9()
  let o10_51 = ClassAvailableOn10_51() // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
  
  o10_9.someMethod()
  o10_51.someMethod()
  
  let _ = o10_9.someProp
  let _ = o10_51.someProp 
}

func castingPotentiallyUnavailableClass(_ o : AnyObject) {
      // expected-note@-1 3{{add @available attribute to enclosing global function}}
  let _ = o as! ClassAvailableOn10_51 // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  let _ = o as? ClassAvailableOn10_51 // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  let _ = o is ClassAvailableOn10_51 // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

protocol Creatable {
  init()
}

@available(OSX, introduced: 10.51)
class ClassAvailableOn10_51_Creatable : Creatable {
  required init() {}
}

func create<T : Creatable>() -> T {
  return T()
}

class ClassWithGenericTypeParameter<T> { }

class ClassWithTwoGenericTypeParameter<T, S> { }

func classViaTypeParameter() {
  // expected-note@-1 9{{add @available attribute to enclosing global function}}
  let _ : ClassAvailableOn10_51_Creatable = // expected-error {{'ClassAvailableOn10_51_Creatable' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
      create()
      
  let _ = create() as
      ClassAvailableOn10_51_Creatable // expected-error {{'ClassAvailableOn10_51_Creatable' is only available in macOS 10.51 or newer}}
          // expected-note@-1 {{add 'if #available' version check}}

  let _ = [ClassAvailableOn10_51]() // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  let _: ClassWithGenericTypeParameter<ClassAvailableOn10_51> = ClassWithGenericTypeParameter() // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  let _: ClassWithTwoGenericTypeParameter<ClassAvailableOn10_51, String> = ClassWithTwoGenericTypeParameter() // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  let _: ClassWithTwoGenericTypeParameter<String, ClassAvailableOn10_51> = ClassWithTwoGenericTypeParameter() // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  let _: ClassWithTwoGenericTypeParameter<ClassAvailableOn10_51, ClassAvailableOn10_51> = ClassWithTwoGenericTypeParameter() // expected-error 2{{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 2{{add 'if #available' version check}}

  let _: ClassAvailableOn10_51? = nil // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

}

// Potentially unavailable class used in declarations

class ClassWithDeclarationsOfPotentiallyUnavailableClasses {
      // expected-note@-1 6{{add @available attribute to enclosing class}}

  @available(OSX, introduced: 10.51)
  init() {}

  var propertyOfPotentiallyUnavailableType: ClassAvailableOn10_51 // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
  
  @available(OSX, introduced: 10.51)
  static var potentiallyUnavailableStaticPropertyOfPotentiallyUnavailableType: ClassAvailableOn10_51 = ClassAvailableOn10_51()

  @available(OSX, introduced: 10.51)
  static var potentiallyUnavailableStaticPropertyOfOptionalPotentiallyUnavailableType: ClassAvailableOn10_51?

  func methodWithPotentiallyUnavailableParameterType(_ o : ClassAvailableOn10_51) { // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add @available attribute to enclosing instance method}}
  }
  
  @available(OSX, introduced: 10.51)
  func potentiallyUnavailableMethodWithPotentiallyUnavailableParameterType(_ o : ClassAvailableOn10_51) {}
  
  func methodWithPotentiallyUnavailableReturnType() -> ClassAvailableOn10_51 { // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 2{{add @available attribute to enclosing instance method}}

    return ClassAvailableOn10_51() // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
  }

  @available(OSX, unavailable)
  func unavailableMethodWithPotentiallyUnavailableParameterType(_ o : ClassAvailableOn10_51) {}
  
  @available(OSX, introduced: 10.51)
  func potentiallyUnavailableMethodWithPotentiallyUnavailableReturnType() -> ClassAvailableOn10_51 {
    return ClassAvailableOn10_51()
  }
  
  @available(OSX, unavailable)
  func unavailableMethodWithPotentiallyUnavailableReturnType() -> ClassAvailableOn10_51 {
    guard #available(OSX 10.51, *) else { fatalError() }
    return ClassAvailableOn10_51()
  }

  func methodWithPotentiallyUnavailableLocalDeclaration() {
      // expected-note@-1 {{add @available attribute to enclosing instance method}}
    let _ : ClassAvailableOn10_51 = methodWithPotentiallyUnavailableReturnType() // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
  }
  
  @available(OSX, introduced: 10.51)
  func potentiallyUnavailableMethodWithPotentiallyUnavailableLocalDeclaration() {
    let _ : ClassAvailableOn10_51 = methodWithPotentiallyUnavailableReturnType()
  }
  
  @available(OSX, unavailable)
  func unavailableMethodWithPotentiallyUnavailableLocalDeclaration() {
    let _ : ClassAvailableOn10_51 = methodWithPotentiallyUnavailableReturnType() // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
  }
}

func referToPotentiallyUnavailableStaticProperty() {
      // expected-note@-1 {{add @available attribute to enclosing global function}}
  let _ = ClassWithDeclarationsOfPotentiallyUnavailableClasses.potentiallyUnavailableStaticPropertyOfPotentiallyUnavailableType // expected-error {{'potentiallyUnavailableStaticPropertyOfPotentiallyUnavailableType' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

class ClassExtendingPotentiallyUnavailableClass : ClassAvailableOn10_51 { // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
    // expected-note@-1 {{add @available attribute to enclosing class}}
}

@available(OSX, introduced: 10.51)
class PotentiallyUnavailableClassExtendingPotentiallyUnavailableClass : ClassAvailableOn10_51 {
}

@available(OSX, unavailable)
class UnavailableClassExtendingPotentiallyUnavailableClass : ClassAvailableOn10_51 {
}

// Method availability is contravariant

class SuperWithAlwaysAvailableMembers {
  func shouldAlwaysBeAvailableMethod() { // expected-note {{overridden declaration is here}}
  }
  
  var shouldAlwaysBeAvailableProperty: Int { // expected-note {{overridden declaration is here}}
    get { return 9 }
    set(newVal) {}
  }

  var setterShouldAlwaysBeAvailableProperty: Int {
    get { return 9 }
    set(newVal) {} // expected-note {{overridden declaration is here}}
  }

  var getterShouldAlwaysBeAvailableProperty: Int {
    get { return 9 } // expected-note {{overridden declaration is here}}
    set(newVal) {}
  }
}

class SubWithLimitedMemberAvailability : SuperWithAlwaysAvailableMembers {
  @available(OSX, introduced: 10.51)
  override func shouldAlwaysBeAvailableMethod() { // expected-error {{overriding 'shouldAlwaysBeAvailableMethod' must be as available as declaration it overrides}}
  }
  
  @available(OSX, introduced: 10.51)
  override var shouldAlwaysBeAvailableProperty: Int { // expected-error {{overriding 'shouldAlwaysBeAvailableProperty' must be as available as declaration it overrides}}
    get { return 10 }
    set(newVal) {}
  }

  override var setterShouldAlwaysBeAvailableProperty: Int {
    get { return 9 }
    @available(OSX, introduced: 10.51)
    set(newVal) {} // expected-error {{overriding setter for 'setterShouldAlwaysBeAvailableProperty' must be as available as declaration it overrides}}
    // This is a terrible diagnostic. rdar://problem/20427938
  }

  override var getterShouldAlwaysBeAvailableProperty: Int {
    @available(OSX, introduced: 10.51)
    get { return 9 } // expected-error {{overriding getter for 'getterShouldAlwaysBeAvailableProperty' must be as available as declaration it overrides}}
    set(newVal) {}
  }
}

@available(OSX, introduced: 10.51)
class SubWithLimitedAvailablility : SuperWithAlwaysAvailableMembers {
  override func shouldAlwaysBeAvailableMethod() {}
  
  override var shouldAlwaysBeAvailableProperty: Int {
    get { return 10 }
    set(newVal) {}
  }
  
  override var setterShouldAlwaysBeAvailableProperty: Int {
    get { return 9 }
    set(newVal) {}
  }

  override var getterShouldAlwaysBeAvailableProperty: Int {
    get { return 9 }
    set(newVal) {}
  }
}

class SuperWithLimitedMemberAvailability {
  @available(OSX, introduced: 10.51)
  func someMethod() {
  }
  
  @available(OSX, introduced: 10.51)
  var someProperty: Int {
    get { return 10 }
    set(newVal) {}
  }
}

class SubWithLargerMemberAvailability : SuperWithLimitedMemberAvailability {
        // expected-note@-1 2{{add @available attribute to enclosing class}}
  @available(OSX, introduced: 10.9)
  override func someMethod() {
    super.someMethod() // expected-error {{'someMethod()' is only available in macOS 10.51 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}
    
    if #available(OSX 10.51, *) {
      super.someMethod()
    }
  }
  
  @available(OSX, introduced: 10.9)
  override var someProperty: Int {
    get { 
      let _ = super.someProperty // expected-error {{'someProperty' is only available in macOS 10.51 or newer}}
          // expected-note@-1 {{add 'if #available' version check}}
      
      if #available(OSX 10.51, *) {
        let _ = super.someProperty
      }
      
      return 9
      }
    set(newVal) {}
  }
}

@available(OSX, introduced: 10.51)
class SubWithLimitedAvailability : SuperWithLimitedMemberAvailability {
  override func someMethod() {
    super.someMethod()
  }
  
  override var someProperty: Int {
    get { super.someProperty }
    set(newVal) { super.someProperty = newVal }
  }
}

@available(OSX, introduced: 10.52)
class SubWithMoreLimitedAvailability : SuperWithLimitedMemberAvailability {
  override func someMethod() {
    super.someMethod()
  }
  
  override var someProperty: Int {
    get { super.someProperty }
    set(newVal) { super.someProperty = newVal }
  }
}

@available(OSX, introduced: 10.52)
class SubWithMoreLimitedAvailabilityAndRedundantMemberAvailability : SuperWithLimitedMemberAvailability {
  @available(OSX, introduced: 10.52)
  override func someMethod() {
    super.someMethod()
  }
  
  @available(OSX, introduced: 10.52)
  override var someProperty: Int {
    get { super.someProperty }
    set(newVal) { super.someProperty = newVal }
  }
}

@available(OSX, unavailable)
class UnavailableSubWithLargerMemberAvailability : SuperWithLimitedMemberAvailability {
  override func someMethod() {
  }
  
  override var someProperty: Int {
    get { return 5 }
    set(newVal) {}
  }
}

// Inheritance and availability

@available(OSX, introduced: 10.51)
protocol ProtocolAvailableOn10_9 {
}

@available(OSX, introduced: 10.51)
protocol ProtocolAvailableOn10_51 {
}

@available(OSX, introduced: 10.9)
protocol ProtocolAvailableOn10_9InheritingFromProtocolAvailableOn10_51 : ProtocolAvailableOn10_51 { // expected-error {{'ProtocolAvailableOn10_51' is only available in macOS 10.51 or newer}}
}

@available(OSX, introduced: 10.51)
protocol ProtocolAvailableOn10_51InheritingFromProtocolAvailableOn10_9 : ProtocolAvailableOn10_9 {
}

@available(OSX, unavailable)
protocol UnavailableProtocolInheritingFromProtocolAvailableOn10_51 : ProtocolAvailableOn10_51 {
}

@available(OSX, introduced: 10.9)
class SubclassAvailableOn10_9OfClassAvailableOn10_51 : ClassAvailableOn10_51 { // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
}

@available(OSX, unavailable)
class UnavailableSubclassOfClassAvailableOn10_51 : ClassAvailableOn10_51 {
}

// We allow nominal types to conform to protocols that are less available than the types themselves.
@available(OSX, introduced: 10.9)
class ClassAvailableOn10_9AdoptingProtocolAvailableOn10_51 : ProtocolAvailableOn10_51 {
}

func castToPotentiallyUnavailableProtocol() {
      // expected-note@-1 2{{add @available attribute to enclosing global function}}
  let o: ClassAvailableOn10_9AdoptingProtocolAvailableOn10_51 = ClassAvailableOn10_9AdoptingProtocolAvailableOn10_51()

  let _: ProtocolAvailableOn10_51 = o // expected-error {{'ProtocolAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  let _ = o as ProtocolAvailableOn10_51 // expected-error {{'ProtocolAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

@available(OSX, introduced: 10.9)
class SubclassAvailableOn10_9OfClassAvailableOn10_51AlsoAdoptingProtocolAvailableOn10_51 : ClassAvailableOn10_51, ProtocolAvailableOn10_51 { // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
}

class SomeGenericClass<T> { }

@available(OSX, introduced: 10.9)
class SubclassAvailableOn10_9OfSomeGenericClassOfProtocolAvailableOn10_51 : SomeGenericClass<ProtocolAvailableOn10_51> { // expected-error {{'ProtocolAvailableOn10_51' is only available in macOS 10.51 or newer}}
}

@available(OSX, unavailable)
class UnavailableSubclassOfSomeGenericClassOfProtocolAvailableOn10_51 : SomeGenericClass<ProtocolAvailableOn10_51> {
}

func GenericWhereClause<T>(_ t: T) where T: ProtocolAvailableOn10_51 { // expected-error * {{'ProtocolAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 * {{add @available attribute to enclosing global function}}
}

@available(OSX, unavailable)
func UnavailableGenericWhereClause<T>(_ t: T) where T: ProtocolAvailableOn10_51 {
}

func GenericSignature<T : ProtocolAvailableOn10_51>(_ t: T) { // expected-error * {{'ProtocolAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 * {{add @available attribute to enclosing global function}}
}

@available(OSX, unavailable)
func UnavailableGenericSignature<T : ProtocolAvailableOn10_51>(_ t: T) {
}

struct GenericType<T> { // expected-note {{add @available attribute to enclosing generic struct}}
  func nonGenericWhereClause() where T : ProtocolAvailableOn10_51 {} // expected-error {{'ProtocolAvailableOn10_51' is only available in macOS 10.51 or newer}}
  // expected-note@-1 {{add @available attribute to enclosing instance method}}
  
  @available(OSX, unavailable)
  func unavailableNonGenericWhereClause() where T : ProtocolAvailableOn10_51 {}

  struct NestedType where T : ProtocolAvailableOn10_51 {} // expected-error {{'ProtocolAvailableOn10_51' is only available in macOS 10.51 or newer}}
  // expected-note@-1 2{{add @available attribute to enclosing struct}}
  
  @available(OSX, unavailable)
  struct UnavailableNestedType where T : ProtocolAvailableOn10_51 {}
}

// Extensions

extension ClassAvailableOn10_51 { } // expected-error {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
    // expected-note@-1 {{add @available attribute to enclosing extension}}

@available(OSX, unavailable)
extension ClassAvailableOn10_51 { }

@available(OSX, introduced: 10.51)
extension ClassAvailableOn10_51 {
  func m() {
      // expected-note@-1 {{add @available attribute to enclosing instance method}}
    let _ = globalFuncAvailableOn10_51()
    let _ = globalFuncAvailableOn10_52() // expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
  }
}

class ClassToExtend { }

@available(OSX, introduced: 10.51)
extension ClassToExtend {

  func extensionMethod() { }

  @available(OSX, introduced: 10.52)
  func extensionMethod10_52() { }

  class ExtensionClass { }

  // We rely on not allowing nesting of extensions, so test to make sure
  // this emits an error.
  // CHECK:error: declaration is only valid at file scope
  extension ClassToExtend { } // expected-error {{declaration is only valid at file scope}}
}

// We allow protocol extensions for protocols that are less available than the
// conforming class.
extension ClassToExtend : ProtocolAvailableOn10_51 {
}

@available(OSX, introduced: 10.51)
extension ClassToExtend { // expected-note {{enclosing scope requires availability of macOS 10.51 or newer}}
  @available(OSX, introduced: 10.9) // expected-error {{instance method cannot be more available than enclosing scope}}
  func extensionMethod10_9() { }
}

func usePotentiallyUnavailableExtension() {
      // expected-note@-1 3{{add @available attribute to enclosing global function}}
  let o = ClassToExtend()

  o.extensionMethod() // expected-error {{'extensionMethod()' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  let _ = ClassToExtend.ExtensionClass() // expected-error {{'ExtensionClass' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  o.extensionMethod10_52() // expected-error {{'extensionMethod10_52()' is only available in macOS 10.52 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

// Availability of synthesized designated initializers.

@available(OSX, introduced: 10.51)
class WidelyAvailableBase {
  init() {}

  @available(OSX, introduced: 10.52)
  init(thing: ()) {}
}

@available(OSX, introduced: 10.53)
class EsotericSmallBatchHipsterThing : WidelyAvailableBase {}

@available(OSX, introduced: 10.53)
class NestedClassTest {
  class InnerClass : WidelyAvailableBase {}
}

// Useless #available(...) checks

func functionWithDefaultAvailabilityAndUselessCheck(_ p: Bool) {
// Default availability reflects minimum deployment: 10.9 and up

  if #available(OSX 10.9, *) { // no-warning
    let _ = globalFuncAvailableOn10_9()
  }
  
  if #available(OSX 10.51, *) { // expected-note {{enclosing scope here}}
    let _ = globalFuncAvailableOn10_51()
    
    if #available(OSX 10.51, *) { // expected-warning {{unnecessary check for 'macOS'; enclosing scope ensures guard will always be true}}
      let _ = globalFuncAvailableOn10_51()
    }
  }

  if #available(OSX 10.9, *) { // expected-note {{enclosing scope here}}
  } else {
    // Make sure we generate a warning about an unnecessary check even if the else branch of if is dead.
    if #available(OSX 10.51, *) { // expected-warning {{unnecessary check for 'macOS'; enclosing scope ensures guard will always be true}}
    }
  }

  // This 'if' is strictly to limit the scope of the guard fallthrough
  if p {
    guard #available(OSX 10.9, *) else { // expected-note {{enclosing scope here}}
      // Make sure we generate a warning about an unnecessary check even if the else branch of guard is dead.
      if #available(OSX 10.51, *) { // expected-warning {{unnecessary check for 'macOS'; enclosing scope ensures guard will always be true}}
      }
    }
  }

  // We don't want * generate a warn about useless checks; the check may be required on
  // another platform
  if #available(iOS 8.0, *) {
  }

  if #available(OSX 10.51, *) {
    // Similarly do not want '*' to generate a warning in a refined TRC.
    if #available(iOS 8.0, *) {
    }
  }
}

@available(OSX, unavailable)
func explicitlyUnavailable() { } // expected-note 2{{'explicitlyUnavailable()' has been explicitly marked unavailable here}}

func functionWithUnavailableInDeadBranch() {

  if #available(iOS 8.0, *) {
  } else {
    // This branch is dead on OSX, so we shouldn't a warning about use of potentially unavailable APIs in it.
    _ = globalFuncAvailableOn10_51() // no-warning

    @available(OSX 10.51, *)
    func localFuncAvailableOn10_51() {
      _ = globalFuncAvailableOn10_52() // no-warning
    }

    localFuncAvailableOn10_51() // no-warning

    explicitlyUnavailable() // expected-error {{'explicitlyUnavailable()' is unavailable}}
  }

  guard #available(iOS 8.0, *) else {
    _ = globalFuncAvailableOn10_51() // no-warning

    explicitlyUnavailable() // expected-error {{'explicitlyUnavailable()' is unavailable}}
  }
}

@available(OSX, introduced: 10.51)
func functionWithSpecifiedAvailabilityAndUselessCheck() { // expected-note 2{{enclosing scope here}}
  if #available(OSX 10.9, *) { // expected-warning {{unnecessary check for 'macOS'; enclosing scope ensures guard will always be true}}
    let _ = globalFuncAvailableOn10_9()
  }
  
  if #available(OSX 10.51, *) { // expected-warning {{unnecessary check for 'macOS'; enclosing scope ensures guard will always be true}}
    let _ = globalFuncAvailableOn10_51()
  }
}

// #available(...) outside if statement guards

func injectToOptional<T>(_ v: T) -> T? {
  return v
}


if let _ = injectToOptional(5), #available(OSX 10.52, *) {}  // ok


// Refining context inside guard

if #available(OSX 10.51, *),
   let _ = injectToOptional(globalFuncAvailableOn10_51()),
   let _ = injectToOptional(globalFuncAvailableOn10_52()) { // expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}

  let _ = globalFuncAvailableOn10_51()
  let _ = globalFuncAvailableOn10_52() // expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}
}

if let _ = injectToOptional(5), #available(OSX 10.51, *),
   let _ = injectToOptional(globalFuncAvailableOn10_51()),
   let _ = injectToOptional(globalFuncAvailableOn10_52()) { // expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}

  let _ = globalFuncAvailableOn10_51()
  let _ = globalFuncAvailableOn10_52() // expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}
}

if let _ = injectToOptional(globalFuncAvailableOn10_51()), #available(OSX 10.51, *), // expected-error {{'globalFuncAvailableOn10_51()' is only available in macOS 10.51 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}
   let _ = injectToOptional(globalFuncAvailableOn10_52()) { // expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}

}

if let _ = injectToOptional(5), #available(OSX 10.51, *), // expected-note {{enclosing scope here}}
   let _ = injectToOptional(6), #available(OSX 10.51, *) { // expected-warning {{unnecessary check for 'macOS'; enclosing scope ensures guard will always be true}}
}


// Tests for the guard control construct.

func useGuardAvailable() {
        // expected-note@-1 3{{add @available attribute to enclosing global function}}
  // Guard fallthrough should refine context
  guard #available(OSX 10.51, *) else { // expected-note {{enclosing scope here}}
    let _ = globalFuncAvailableOn10_51() // expected-error {{'globalFuncAvailableOn10_51()' is only available in macOS 10.51 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}
    return
  }

  let _ = globalFuncAvailableOn10_51()

  let _ = globalFuncAvailableOn10_52() // expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}

  if #available(OSX 10.51, *) { // expected-warning {{unnecessary check for 'macOS'; enclosing scope ensures guard will always be true}}
  }

  if globalFuncAvailableOn10_51() > 0 {
    guard #available(OSX 10.52, *),
            let x = injectToOptional(globalFuncAvailableOn10_52()) else { return }
    _ = x
  }

  let _ = globalFuncAvailableOn10_52() // expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}
}

func twoGuardsInSameBlock(_ p: Int) {
        // expected-note@-1 {{add @available attribute to enclosing global function}}
  if (p > 0) {
    guard #available(OSX 10.51, *) else { return }

    let _ = globalFuncAvailableOn10_51()

    guard #available(OSX 10.52, *) else { return }

    let _ = globalFuncAvailableOn10_52()
  }

  let _ = globalFuncAvailableOn10_51() // expected-error {{'globalFuncAvailableOn10_51()' is only available in macOS 10.51 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}
}

// Refining while loops

while globalFuncAvailableOn10_51() > 10 { } // expected-error {{'globalFuncAvailableOn10_51()' is only available in macOS 10.51 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}

while #available(OSX 10.51, *), // expected-note {{enclosing scope here}}
      globalFuncAvailableOn10_51() > 10 {

  let _ = globalFuncAvailableOn10_51()

  let _ = globalFuncAvailableOn10_52() // expected-error {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
        // expected-note@-1 {{add 'if #available' version check}}

  while globalFuncAvailableOn10_51() > 11,
        let _ = injectToOptional(5),
        #available(OSX 10.52, *) {
    let _ = globalFuncAvailableOn10_52();
  }

  while #available(OSX 10.51, *) { // expected-warning {{unnecessary check for 'macOS'; enclosing scope ensures guard will always be true}}
  }
}

// Tests for Fix-It replacement text
// The whitespace in the replacement text is particularly important here -- it reflects the level
// of indentation for the added if #available() or @available attribute. Note that, for the moment, we hard
// code *added* indentation in Fix-Its as 4 spaces (that is, when indenting in a Fix-It, we
// take whatever indentation was there before and add 4 spaces to it).

functionAvailableOn10_51()
    // expected-error@-1 {{'functionAvailableOn10_51()' is only available in macOS 10.51 or newer}}
    // expected-note@-2 {{add 'if #available' version check}} {{1-27=if #available(macOS 10.51, *) {\n    functionAvailableOn10_51()\n} else {\n    // Fallback on earlier versions\n}}}

let declForFixitAtTopLevel: ClassAvailableOn10_51? = nil
      // expected-error@-1 {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-2 {{add 'if #available' version check}} {{1-57=if #available(macOS 10.51, *) {\n    let declForFixitAtTopLevel: ClassAvailableOn10_51? = nil\n} else {\n    // Fallback on earlier versions\n}}}

func fixitForReferenceInGlobalFunction() {
      // expected-note@-1 {{add @available attribute to enclosing global function}} {{1-1=@available(macOS 10.51, *)\n}}
  functionAvailableOn10_51()
      // expected-error@-1 {{'functionAvailableOn10_51()' is only available in macOS 10.51 or newer}}
      // expected-note@-2 {{add 'if #available' version check}} {{3-29=if #available(macOS 10.51, *) {\n      functionAvailableOn10_51()\n  } else {\n      // Fallback on earlier versions\n  }}}
      
}

public func fixitForReferenceInGlobalFunctionWithDeclModifier() {
      // expected-note@-1 {{add @available attribute to enclosing global function}} {{1-1=@available(macOS 10.51, *)\n}}
  functionAvailableOn10_51()
      // expected-error@-1 {{'functionAvailableOn10_51()' is only available in macOS 10.51 or newer}}
      // expected-note@-2 {{add 'if #available' version check}} {{3-29=if #available(macOS 10.51, *) {\n      functionAvailableOn10_51()\n  } else {\n      // Fallback on earlier versions\n  }}}
      
}

func fixitForReferenceInGlobalFunctionWithAttribute() -> Never {
    // expected-note@-1 {{add @available attribute to enclosing global function}} {{1-1=@available(macOS 10.51, *)\n}}
  _ = 0 // Avoid treating the call to functionAvailableOn10_51 as an implicit return
  functionAvailableOn10_51()
    // expected-error@-1 {{'functionAvailableOn10_51()' is only available in macOS 10.51 or newer}}
    // expected-note@-2 {{add 'if #available' version check}} {{3-29=if #available(macOS 10.51, *) {\n      functionAvailableOn10_51()\n  } else {\n      // Fallback on earlier versions\n  }}}
    
}

func takesAutoclosure(_ c : @autoclosure () -> ()) {
}

class ClassForFixit {
        // expected-note@-1 12{{add @available attribute to enclosing class}} {{1-1=@available(macOS 10.51, *)\n}}
  func fixitForReferenceInMethod() {
        // expected-note@-1 {{add @available attribute to enclosing instance method}} {{3-3=@available(macOS 10.51, *)\n  }}
    functionAvailableOn10_51()
        // expected-error@-1 {{'functionAvailableOn10_51()' is only available in macOS 10.51 or newer}}
        // expected-note@-2 {{add 'if #available' version check}} {{5-31=if #available(macOS 10.51, *) {\n        functionAvailableOn10_51()\n    } else {\n        // Fallback on earlier versions\n    }}}
  }

  func fixitForReferenceNestedInMethod() {
          // expected-note@-1 3{{add @available attribute to enclosing instance method}} {{3-3=@available(macOS 10.51, *)\n  }}
    func inner() {
      functionAvailableOn10_51()
          // expected-error@-1 {{'functionAvailableOn10_51()' is only available in macOS 10.51 or newer}}
          // expected-note@-2 {{add 'if #available' version check}} {{7-33=if #available(macOS 10.51, *) {\n          functionAvailableOn10_51()\n      } else {\n          // Fallback on earlier versions\n      }}}
    }

    let _: () -> () = { () in
      functionAvailableOn10_51()
          // expected-error@-1 {{'functionAvailableOn10_51()' is only available in macOS 10.51 or newer}}
          // expected-note@-2 {{add 'if #available' version check}} {{7-33=if #available(macOS 10.51, *) {\n          functionAvailableOn10_51()\n      } else {\n          // Fallback on earlier versions\n      }}}
    }

    takesAutoclosure(functionAvailableOn10_51())
          // expected-error@-1 {{'functionAvailableOn10_51()' is only available in macOS 10.51 or newer}}
          // expected-note@-2 {{add 'if #available' version check}} {{5-49=if #available(macOS 10.51, *) {\n        takesAutoclosure(functionAvailableOn10_51())\n    } else {\n        // Fallback on earlier versions\n    }}}
          
  }

  var fixitForReferenceInPropertyAccessor: Int {
        // expected-note@-1 {{add @available attribute to enclosing property}} {{3-3=@available(macOS 10.51, *)\n  }}
    get {
      functionAvailableOn10_51()
        // expected-error@-1 {{'functionAvailableOn10_51()' is only available in macOS 10.51 or newer}}
        // expected-note@-2 {{add 'if #available' version check}} {{7-33=if #available(macOS 10.51, *) {\n          functionAvailableOn10_51()\n      } else {\n          // Fallback on earlier versions\n      }}}
        
      return 5
    }
  }

  var fixitForReferenceInPropertyType: ClassAvailableOn10_51? = nil
      // expected-error@-1 {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}

  lazy var fixitForReferenceInLazyPropertyType: ClassAvailableOn10_51? = nil
      // expected-error@-1 {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}

  private lazy var fixitForReferenceInPrivateLazyPropertyType: ClassAvailableOn10_51? = nil
      // expected-error@-1 {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}

  lazy private var fixitForReferenceInLazyPrivatePropertyType: ClassAvailableOn10_51? = nil
      // expected-error@-1 {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}

  static var fixitForReferenceInStaticPropertyType: ClassAvailableOn10_51? = nil
      // expected-error@-1 {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-2 {{add @available attribute to enclosing static property}} {{3-3=@available(macOS 10.51, *)\n  }}

  var fixitForReferenceInPropertyTypeMultiple: ClassAvailableOn10_51? = nil, other: Int = 7
      // expected-error@-1 {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}

  func fixitForRefInGuardOfIf() {
        // expected-note@-1 {{add @available attribute to enclosing instance method}} {{3-3=@available(macOS 10.51, *)\n  }}
    if (globalFuncAvailableOn10_51() > 1066) {
      let _ = 5
      let _ = 6
    }
        // expected-error@-4 {{'globalFuncAvailableOn10_51()' is only available in macOS 10.51 or newer}}
        // expected-note@-5 {{add 'if #available' version check}} {{5-+3:6=if #available(macOS 10.51, *) {\n        if (globalFuncAvailableOn10_51() > 1066) {\n          let _ = 5\n          let _ = 6\n        }\n    } else {\n        // Fallback on earlier versions\n    }}}
  }
}

extension ClassToExtend {
        // expected-note@-1 {{add @available attribute to enclosing extension}}
  func fixitForReferenceInExtensionMethod() {
        // expected-note@-1 {{add @available attribute to enclosing instance method}} {{3-3=@available(macOS 10.51, *)\n  }}
    functionAvailableOn10_51()
        // expected-error@-1 {{'functionAvailableOn10_51()' is only available in macOS 10.51 or newer}}
        // expected-note@-2 {{add 'if #available' version check}} {{5-31=if #available(macOS 10.51, *) {\n        functionAvailableOn10_51()\n    } else {\n        // Fallback on earlier versions\n    }}}
  }
}

enum EnumForFixit {
      // expected-note@-1 2{{add @available attribute to enclosing enum}} {{1-1=@available(macOS 10.51, *)\n}}
  case CaseWithPotentiallyUnavailablePayload(p: ClassAvailableOn10_51)
      // expected-error@-1 {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}

  case CaseWithPotentiallyUnavailablePayload2(p: ClassAvailableOn10_51), WithoutPayload
      // expected-error@-1 {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      
}

@objc
class Y {
 var z = 0
}

@objc
class X {
  @objc var y = Y()
}

func testForFixitWithNestedMemberRefExpr() {
    // expected-note@-1 2{{add @available attribute to enclosing global function}} {{1-1=@available(macOS 10.52, *)\n}}
  let x = X()

  x.y.z = globalFuncAvailableOn10_52()
      // expected-error@-1 {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
      // expected-note@-2 {{add 'if #available' version check}} {{3-39=if #available(macOS 10.52, *) {\n      x.y.z = globalFuncAvailableOn10_52()\n  } else {\n      // Fallback on earlier versions\n  }}}

  // Access via dynamic member reference
  let anyX: AnyObject = x
  anyX.y?.z = globalFuncAvailableOn10_52()
      // expected-error@-1 {{'globalFuncAvailableOn10_52()' is only available in macOS 10.52 or newer}}
      // expected-note@-2 {{add 'if #available' version check}} {{3-43=if #available(macOS 10.52, *) {\n      anyX.y?.z = globalFuncAvailableOn10_52()\n  } else {\n      // Fallback on earlier versions\n  }}}
      
}

// Protocol Conformances

protocol ProtocolWithRequirementMentioningPotentiallyUnavailable {
      // expected-note@-1 2{{add @available attribute to enclosing protocol}}
  func hasPotentiallyUnavailableParameter(_ p: ClassAvailableOn10_51) // expected-error * {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 * {{add @available attribute to enclosing instance method}}
      

  func hasPotentiallyUnavailableReturn() -> ClassAvailableOn10_51 // expected-error * {{'ClassAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 * {{add @available attribute to enclosing instance method}}

  @available(OSX 10.51, *)
  func hasPotentiallyUnavailableWithAnnotation(_ p: ClassAvailableOn10_51) -> ClassAvailableOn10_51
}

protocol HasMethodF {
  associatedtype T
  func f(_ p: T) // expected-note 3{{protocol requirement here}}
}

class TriesToConformWithFunctionIntroducedOn10_51 : HasMethodF {
  @available(OSX, introduced: 10.51)
  func f(_ p: Int) { } // expected-error {{protocol 'HasMethodF' requires 'f' to be available in macOS 10.50 and newer}}
}


class ConformsWithFunctionIntroducedOnMinimumDeploymentTarget : HasMethodF {
  // Even though this function is less available than its requirement,
  // it is available on a deployment targets, so the conformance is safe.
  @available(OSX, introduced: 10.9)
  func f(_ p: Int) { }
}

class SuperHasMethodF {
  @available(OSX, introduced: 10.51)
    func f(_ p: Int) { } // expected-note {{'f' declared here}}
}

class TriesToConformWithPotentiallyUnavailableFunctionInSuperClass : SuperHasMethodF, HasMethodF { // expected-error {{protocol 'HasMethodF' requires 'f' to be available in macOS 10.50 and newer}}
  // The conformance here is generating an error on f in the super class.
}

@available(OSX, introduced: 10.51)
class ConformsWithPotentiallyUnavailableFunctionInSuperClass : SuperHasMethodF, HasMethodF {
  // Limiting this class to only be available on 10.51 and newer means that
  // the witness in SuperHasMethodF is safe for the requirement on HasMethodF.
  // in order for this class to be referenced we must be running on 10.51 or
  // greater.
}

class ConformsByOverridingFunctionInSuperClass : SuperHasMethodF, HasMethodF {
  // Now the witness is this f() (which is always available) and not the f()
  // from the super class, so conformance is safe.
  override func f(_ p: Int) { }
}


// Attempt to conform in protocol extension with unavailable witness
// in extension
class HasNoMethodF1 { }
extension HasNoMethodF1 : HasMethodF {
  @available(OSX, introduced: 10.51)
  func f(_ p: Int) { } // expected-error {{protocol 'HasMethodF' requires 'f' to be available in macOS 10.50 and newer}}
}

class HasNoMethodF2 { }
@available(OSX, introduced: 10.51)
extension HasNoMethodF2 : HasMethodF {
  // This is OK, because the conformance was introduced by an extension.
  func f(_ p: Int) { }
}

@available(OSX, introduced: 10.51)
class HasNoMethodF3 { }
@available(OSX, introduced: 10.51)
extension HasNoMethodF3 : HasMethodF {
  // We expect this conformance to succeed because on every version where HasNoMethodF3
  // is available, HasNoMethodF3's f() is as available as the protocol requirement
  func f(_ p: Int) { }
}

@available(OSX, introduced: 10.51)
protocol HasMethodFOn10_51 {
  func f(_ p: Int)
}

class ConformsToPotentiallyUnavailableProtocolWithPotentiallyUnavailableWitness : HasMethodFOn10_51 {
  @available(OSX, introduced: 10.51)
  func f(_ p: Int) { }
}

@available(OSX, introduced: 10.51)
class HasNoMethodF4 { }
@available(OSX, introduced: 10.52)
extension HasNoMethodF4 : HasMethodFOn10_51 {
  // This is OK, because the conformance was introduced by an extension.
  func f(_ p: Int) { }
}

@available(OSX, introduced: 10.51)
protocol HasTakesClassAvailableOn10_51 {
  func takesClassAvailableOn10_51(_ o: ClassAvailableOn10_51) // expected-note 2{{protocol requirement here}}
}

class AttemptsToConformToHasTakesClassAvailableOn10_51 : HasTakesClassAvailableOn10_51 {
  @available(OSX, introduced: 10.52)
  func takesClassAvailableOn10_51(_ o: ClassAvailableOn10_51) { // expected-error {{protocol 'HasTakesClassAvailableOn10_51' requires 'takesClassAvailableOn10_51' to be available in macOS 10.51 and newer}}
  }
}

class ConformsToHasTakesClassAvailableOn10_51 : HasTakesClassAvailableOn10_51 {
  @available(OSX, introduced: 10.51)
  func takesClassAvailableOn10_51(_ o: ClassAvailableOn10_51) {
  }
}

class TakesClassAvailableOn10_51_A { }
extension TakesClassAvailableOn10_51_A : HasTakesClassAvailableOn10_51 {
  @available(OSX, introduced: 10.52)
  func takesClassAvailableOn10_51(_ o: ClassAvailableOn10_51) { // expected-error {{protocol 'HasTakesClassAvailableOn10_51' requires 'takesClassAvailableOn10_51' to be available in macOS 10.51 and newer}}
  }
}

class TakesClassAvailableOn10_51_B { }
extension TakesClassAvailableOn10_51_B : HasTakesClassAvailableOn10_51 {
  @available(OSX, introduced: 10.51)
  func takesClassAvailableOn10_51(_ o: ClassAvailableOn10_51) {
  }
}


// We want conditional availability to play a role in picking a witness for a
// protocol requirement.
class TestAvailabilityAffectsWitnessCandidacy : HasMethodF {
  // Test that we choose the less specialized witness, because the more specialized
  // witness is conditionally unavailable.

  @available(OSX, introduced: 10.51)
  func f(_ p: Int) { }

  func f<T>(_ p: T) { }
}

protocol HasPotentiallyUnavailableMethodF {
  @available(OSX, introduced: 10.51)
  func f(_ p: String)
}

class ConformsWithPotentiallyUnavailableFunction : HasPotentiallyUnavailableMethodF {
  @available(OSX, introduced: 10.9)
  func f(_ p: String) { }
}

func usePotentiallyUnavailableProtocolMethod(_ h: HasPotentiallyUnavailableMethodF) {
      // expected-note@-1 {{add @available attribute to enclosing global function}}
  h.f("Foo") // expected-error {{'f' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

func usePotentiallyUnavailableProtocolMethod<H : HasPotentiallyUnavailableMethodF> (_ h: H) {
      // expected-note@-1 {{add @available attribute to enclosing global function}}
  h.f("Foo") // expected-error {{'f' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}


// Short-form @available() annotations

@available(OSX 10.51, *)
class ClassWithShortFormAvailableOn10_51 {
}

@available(OSX 10.53, *)
class ClassWithShortFormAvailableOn10_53 {
}

@available(OSX 10.54, *)
class ClassWithShortFormAvailableOn10_54 {
}

@available(OSX 10.9, *)
func funcWithShortFormAvailableOn10_9() {
  let _ = ClassWithShortFormAvailableOn10_51() // expected-error {{'ClassWithShortFormAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

@available(OSX 10.51, *)
func funcWithShortFormAvailableOn10_51() {
  let _ = ClassWithShortFormAvailableOn10_51()
}

@available(iOS 14.0, *)
func funcWithShortFormAvailableOniOS14() {
  // expected-note@-1 {{add @available attribute to enclosing global function}}
  let _ = ClassWithShortFormAvailableOn10_51() // expected-error {{'ClassWithShortFormAvailableOn10_51' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

@available(iOS 14.0, OSX 10.53, *)
func funcWithShortFormAvailableOniOS14AndOSX10_53() {
  let _ = ClassWithShortFormAvailableOn10_51()
}

// Not idiomatic but we need to be able to handle it.
@available(iOS 8.0, *)
@available(OSX 10.51, *)
func funcWithMultipleShortFormAnnotationsForDifferentPlatforms() {
  let _ = ClassWithShortFormAvailableOn10_51()
}

@available(OSX 10.51, *)
@available(OSX 10.53, *)
@available(OSX 10.52, *)
func funcWithMultipleShortFormAnnotationsForTheSamePlatform() {
  let _ = ClassWithShortFormAvailableOn10_53()

  let _ = ClassWithShortFormAvailableOn10_54() // expected-error {{'ClassWithShortFormAvailableOn10_54' is only available in macOS 10.54 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

func useShortFormAvailable() {
  // expected-note@-1 4{{add @available attribute to enclosing global function}}

  funcWithShortFormAvailableOn10_9()

  funcWithShortFormAvailableOn10_51() // expected-error {{'funcWithShortFormAvailableOn10_51()' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  funcWithShortFormAvailableOniOS14()

  funcWithShortFormAvailableOniOS14AndOSX10_53() // expected-error {{'funcWithShortFormAvailableOniOS14AndOSX10_53()' is only available in macOS 10.53 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  funcWithMultipleShortFormAnnotationsForDifferentPlatforms() // expected-error {{'funcWithMultipleShortFormAnnotationsForDifferentPlatforms()' is only available in macOS 10.51 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}

  funcWithMultipleShortFormAnnotationsForTheSamePlatform() // expected-error {{'funcWithMultipleShortFormAnnotationsForTheSamePlatform()' is only available in macOS 10.53 or newer}}
      // expected-note@-1 {{add 'if #available' version check}}
}

// Unavailability takes precedence over availability and is inherited

@available(OSX 10.9, *)
@available(OSX, unavailable)
func unavailableWins() { }
    // expected-note@-1 {{'unavailableWins()' has been explicitly marked unavailable here}}

struct HasUnavailableExtension {
  @available(OSX, unavailable)
  public func directlyUnavailable() { }
      // expected-note@-1 {{'directlyUnavailable()' has been explicitly marked unavailable here}}
}

@available(OSX, unavailable)
extension HasUnavailableExtension {

  public func inheritsUnavailable() { }
      // expected-note@-1 {{'inheritsUnavailable()' has been explicitly marked unavailable here}}

  @available(OSX 10.9, *)
  public func moreAvailableButStillUnavailable() { }
      // expected-note@-1 {{'moreAvailableButStillUnavailable()' has been explicitly marked unavailable here}}
}

func useHasUnavailableExtension(_ s: HasUnavailableExtension) {
  unavailableWins() // expected-error {{'unavailableWins()' is unavailable}}
  s.directlyUnavailable() // expected-error {{'directlyUnavailable()' is unavailable}}
  s.inheritsUnavailable() // expected-error {{'inheritsUnavailable()' is unavailable in macOS}}
  s.moreAvailableButStillUnavailable() // expected-error {{'moreAvailableButStillUnavailable()' is unavailable in macOS}}
}

@available(macOS 10.15, *)
func f() -> Int { 17 }

class StoredPropertiesWithAvailabilityInClosures {
  private static let value: Int = {
    if #available(macOS 10.15, *) {
      return f()
    }

    return 0
  }()

  @available(macOS 10.14, *)
  private static let otherValue: Int = {
    if #available(macOS 10.15, *) {
      return f()
    }

    return 0
  }()
}
