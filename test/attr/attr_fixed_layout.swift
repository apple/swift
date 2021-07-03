// RUN: %target-swift-frontend -typecheck -swift-version 4.2 -verify -dump-ast -enable-library-evolution -enable-access-control-hacks %s | %FileCheck --check-prefix=RESILIENCE-ON %s
// RUN: %target-swift-frontend -typecheck -swift-version 4.2 -verify -dump-ast -enable-library-evolution -enable-access-control-hacks -enable-testing %s | %FileCheck --check-prefix=RESILIENCE-ON %s
// RUN: not %target-swift-frontend -typecheck -swift-version 4.2 -enable-access-control-hacks -dump-ast %s | %FileCheck --check-prefix=RESILIENCE-OFF %s
// RUN: not %target-swift-frontend -typecheck -swift-version 4.2 -enable-access-control-hacks -dump-ast %s -enable-testing | %FileCheck --check-prefix=RESILIENCE-OFF %s

//
// Public types with @frozen are always fixed layout
//

// RESILIENCE-ON: struct_decl{{.*}}"Point" interface type='Point.Type' access=public non-resilient
// RESILIENCE-OFF: struct_decl{{.*}}"Point" interface type='Point.Type' access=public non-resilient
@frozen public struct Point {
  let x, y: Int
}

// RESILIENCE-ON: struct_decl{{.*}}"FixedPoint" interface type='FixedPoint.Type' access=public non-resilient
// RESILIENCE-OFF: struct_decl{{.*}}"FixedPoint" interface type='FixedPoint.Type' access=public non-resilient
@_fixed_layout public struct FixedPoint {
  // expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
  let x, y: Int
}

// RESILIENCE-ON: enum_decl{{.*}}"ChooseYourOwnAdventure" interface type='ChooseYourOwnAdventure.Type' access=public non-resilient
// RESILIENCE-OFF: enum_decl{{.*}}"ChooseYourOwnAdventure" interface type='ChooseYourOwnAdventure.Type' access=public non-resilient
@frozen public enum ChooseYourOwnAdventure {
  case JumpIntoRabbitHole
  case EatMushroom
}

//
// Public types are resilient when -enable-library-evolution is on
//

// RESILIENCE-ON: struct_decl{{.*}}"Size" interface type='Size.Type' access=public resilient
// RESILIENCE-OFF: struct_decl{{.*}}"Size" interface type='Size.Type' access=public non-resilient
public struct Size {
  let w, h: Int
}

// RESILIENCE-ON: struct_decl{{.*}}"UsableFromInlineStruct" interface type='UsableFromInlineStruct.Type' access=internal non-resilient
// RESILIENCE-OFF: struct_decl{{.*}}"UsableFromInlineStruct" interface type='UsableFromInlineStruct.Type' access=internal non-resilient
@frozen @usableFromInline struct UsableFromInlineStruct {}

// RESILIENCE-ON: struct_decl{{.*}}"UsableFromInlineFixedStruct" interface type='UsableFromInlineFixedStruct.Type' access=internal non-resilient
// RESILIENCE-OFF: struct_decl{{.*}}"UsableFromInlineFixedStruct" interface type='UsableFromInlineFixedStruct.Type' access=internal non-resilient
@_fixed_layout @usableFromInline struct UsableFromInlineFixedStruct {}
// expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}

// RESILIENCE-ON: enum_decl{{.*}}"TaxCredit" interface type='TaxCredit.Type' access=public resilient
// RESILIENCE-OFF: enum_decl{{.*}}"TaxCredit" interface type='TaxCredit.Type' access=public non-resilient
public enum TaxCredit {
  case EarnedIncome
  case MortgageDeduction
}

//
// Internal types are always fixed layout
//

// RESILIENCE-ON: struct_decl{{.*}}"Rectangle" interface type='Rectangle.Type' access=internal non-resilient
// RESILIENCE-OFF: struct_decl{{.*}}"Rectangle" interface type='Rectangle.Type' access=internal non-resilient
struct Rectangle {
  let topLeft: Point
  let bottomRight: Size
}

//
// Diagnostics
//

@frozen struct InternalStruct { // expected-note * {{declared here}}
// expected-error@-1 {{'@frozen' attribute can only be applied to '@usableFromInline' or public declarations, but 'InternalStruct' is internal}}

  @frozen public struct NestedStruct {}
}

@_fixed_layout struct FixedInternalStruct { // expected-note * {{declared here}}
// expected-error@-1 {{'@_fixed_layout' attribute can only be applied to '@usableFromInline' or public declarations, but 'FixedInternalStruct' is internal}}
// expected-warning@-2 {{'@frozen' attribute is now used for fixed-layout structs}}

  @_fixed_layout public struct NestedStruct {}
  // expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
}

@frozen fileprivate struct FileprivateStruct {}
// expected-error@-1 {{'@frozen' attribute can only be applied to '@usableFromInline' or public declarations, but 'FileprivateStruct' is fileprivate}}

@_fixed_layout fileprivate struct FixedFileprivateStruct {}
// expected-error@-1 {{'@_fixed_layout' attribute can only be applied to '@usableFromInline' or public declarations, but 'FixedFileprivateStruct' is fileprivate}}
// expected-warning@-2 {{'@frozen' attribute is now used for fixed-layout structs}}

@frozen private struct PrivateStruct {} // expected-note * {{declared here}}
// expected-error@-1 {{'@frozen' attribute can only be applied to '@usableFromInline' or public declarations, but 'PrivateStruct' is private}}

@_fixed_layout private struct FixedPrivateStruct {} // expected-note * {{declared here}}
// expected-error@-1 {{'@_fixed_layout' attribute can only be applied to '@usableFromInline' or public declarations, but 'FixedPrivateStruct' is private}}
// expected-warning@-2 {{'@frozen' attribute is now used for fixed-layout structs}}


@frozen public struct BadFields1 {
  private var field: PrivateStruct // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
}

@_fixed_layout public struct FixedBadFields1 {
// expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
  private var field: PrivateStruct // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
}

@frozen public struct BadFields2 {
  private var field: PrivateStruct? // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
}

@_fixed_layout public struct FixedBadFields2 {
// expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
  private var field: PrivateStruct? // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
}

@frozen public struct BadFields3 {
  internal var field: InternalStruct? // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
}

@_fixed_layout public struct FixedBadFields3 {
// expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
  internal var field: InternalStruct? // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
}

@frozen @usableFromInline struct BadFields4 {
  internal var field: InternalStruct? // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
}

@_fixed_layout @usableFromInline struct FixedBadFields4 {
// expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
  internal var field: InternalStruct? // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
}

@frozen public struct BadFields5 {
  private var field: PrivateStruct? { // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
    didSet {}
    
    
  }
}

@_fixed_layout public struct FixedBadFields5 {
// expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
  private var field: PrivateStruct? { // expected-error {{type referenced from a stored property in a '@frozen' struct must be '@usableFromInline' or public}}
    didSet {}
    
    
  }
}

// expected-warning@+1 {{the result of a '@usableFromInline' function should be '@usableFromInline' or public}}
@usableFromInline func notReallyUsableFromInline() -> InternalStruct? { return nil }
@frozen public struct BadFields6 {
  private var field = notReallyUsableFromInline() // expected-error {{type referenced from a stored property with inferred type 'InternalStruct?' in a '@frozen' struct must be '@usableFromInline' or public}}
}

// expected-warning@+1 {{the result of a '@usableFromInline' function should be '@usableFromInline' or public}}
@usableFromInline func notReallyUsableFromInlineFixed() -> FixedInternalStruct? { return nil }
@_fixed_layout public struct FrozenBadFields6 {
// expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
  private var field = notReallyUsableFromInlineFixed() // expected-error {{type referenced from a stored property with inferred type 'FixedInternalStruct?' in a '@frozen' struct must be '@usableFromInline' or public}}
}

@frozen public struct OKFields {
  private var publicTy: Size
  internal var ufiTy: UsableFromInlineStruct?

  internal static var staticProp: InternalStruct?

  private var computed: PrivateStruct? { return nil }
}

@_fixed_layout public struct FixedOKFields {
// expected-warning@-1 {{'@frozen' attribute is now used for fixed-layout structs}}
  private var publicTy: Size
  internal var ufiTy: UsableFromInlineStruct?

  internal static var staticProp: InternalStruct?

  private var computed: PrivateStruct? { return nil }
}
