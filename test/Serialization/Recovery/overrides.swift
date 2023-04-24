// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t -module-name Lib -I %S/Inputs/custom-modules %s

// RUN: %target-swift-ide-test -source-filename=x -print-module -module-to-print Lib -I %t -I %S/Inputs/custom-modules | %FileCheck %s

// RUN: %target-swift-ide-test -source-filename=x -print-module -module-to-print Lib -I %t -I %S/Inputs/custom-modules -Xcc -DBAD | %FileCheck -check-prefix CHECK-RECOVERY %s

// RUN: %target-swift-frontend -typecheck %s -I %t -I %S/Inputs/custom-modules -Xcc -DBAD -D TEST -verify

// REQUIRES: objc_interop

#if TEST
import Lib

func testInitializers() {
  _ = D1_DesignatedInitDisappears()
  _ = D1_DesignatedInitDisappears(value: 0) // expected-error {{incorrect argument label in call}}
  _ = D1_DesignatedInitDisappears(convenience: 0)

  _ = D2_OnlyDesignatedInitDisappears(value: 0) // expected-error {{cannot be constructed because it has no accessible initializers}}
  _ = D2_OnlyDesignatedInitDisappears(convenience: 0) // expected-error {{cannot be constructed because it has no accessible initializers}}

  _ = D3_ConvenienceInitDisappears()
  _ = D3_ConvenienceInitDisappears(value: 0) // expected-error {{incorrect argument label in call}}
  _ = D3_ConvenienceInitDisappears(convenience: 0)

  _ = D4_UnknownInitDisappears()
  _ = D4_UnknownInitDisappears(value: 0) // expected-error {{argument passed to call that takes no arguments}}

  // FIXME: Why does 'init()' show up in the generated interface if it can't be
  // called?
  _ = D5_OnlyUnknownInitDisappears() // expected-error {{cannot be constructed because it has no accessible initializers}}
  _ = D5_OnlyUnknownInitDisappears(value: 0) // expected-error {{cannot be constructed because it has no accessible initializers}}
}

func testSubclassInitializers() {
  class DesignatedInitDisappearsSub : D1_DesignatedInitDisappears {}
  _ = DesignatedInitDisappearsSub()
  _ = DesignatedInitDisappearsSub(value: 0) // expected-error {{argument passed to call that takes no arguments}}
  _ = DesignatedInitDisappearsSub(convenience: 0) // expected-error {{argument passed to call that takes no arguments}}

  class OnlyDesignatedInitDisappearsSub : D2_OnlyDesignatedInitDisappears {}
  _ = OnlyDesignatedInitDisappearsSub(value: 0) // expected-error {{cannot be constructed because it has no accessible initializers}}
  _ = OnlyDesignatedInitDisappearsSub(convenience: 0) // expected-error {{cannot be constructed because it has no accessible initializers}}

  class ConvenienceInitDisappearsSub : D3_ConvenienceInitDisappears {}
  _ = ConvenienceInitDisappearsSub()
  _ = ConvenienceInitDisappearsSub(value: 0) // expected-error {{incorrect argument label in call}}
  _ = ConvenienceInitDisappearsSub(convenience: 0) // still inheritable

  class UnknownInitDisappearsSub : D4_UnknownInitDisappears {}
  _ = UnknownInitDisappearsSub()
  _ = UnknownInitDisappearsSub(value: 0) // expected-error {{argument passed to call that takes no arguments}}

  class OnlyUnknownInitDisappearsSub : D5_OnlyUnknownInitDisappears {}
  _ = OnlyUnknownInitDisappearsSub() // expected-error {{cannot be constructed because it has no accessible initializers}}
  _ = OnlyUnknownInitDisappearsSub(value: 0) // expected-error {{cannot be constructed because it has no accessible initializers}}
}

#else // TEST

import Overrides

// Please use prefixes to keep the printed parts of this file in alphabetical
// order.

public class SwiftOnlyClass {}

public class A_Sub: Base {
  public override func disappearingMethod() {}
  public override func nullabilityChangeMethod() -> Any? { return nil }
  public override func typeChangeMethod() -> Any { return self }
  public override func disappearingMethodWithOverload() {}
  public override var disappearingProperty: Int { return 0 }
  public override var disappearingPropertySetter: Int {
    get { return 0 }
    set {}
  }
}

public class A_Sub2: A_Sub {
  public override func disappearingMethod() {}
}

public final class A_Sub3Final: Base {
  public override func disappearingMethod() {}
  public override func nullabilityChangeMethod() -> Any? { return nil }
  public override func typeChangeMethod() -> Any { return self }
  public override func disappearingMethodWithOverload() {}
  public override var disappearingProperty: Int { return 0 }
  public override var disappearingPropertySetter: Int {
    get { return 0 }
    set {}
  }
}


// CHECK-LABEL: class A_Sub : Base {
// CHECK-NEXT: func disappearingMethod()
// CHECK-NEXT: func nullabilityChangeMethod() -> Any?
// CHECK-NEXT: func typeChangeMethod() -> Any
// CHECK-NEXT: func disappearingMethodWithOverload()
// CHECK-NEXT: var disappearingProperty: Int { get }
// CHECK-NEXT: var disappearingPropertySetter: Int{{$}}
// CHECK-NEXT: init()
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-LABEL: class A_Sub2 : A_Sub {
// CHECK-NEXT: func disappearingMethod()
// CHECK-NEXT: init()
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-LABEL: final class A_Sub3Final : Base {
// CHECK-NEXT: func disappearingMethod()
// CHECK-NEXT: func nullabilityChangeMethod() -> Any?
// CHECK-NEXT: func typeChangeMethod() -> Any
// CHECK-NEXT: func disappearingMethodWithOverload()
// CHECK-NEXT: var disappearingProperty: Int { get }
// CHECK-NEXT: var disappearingPropertySetter: Int{{$}}
// CHECK-NEXT: init()
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class A_Sub : Base {
// CHECK-RECOVERY-NEXT: func disappearingMethod()
// CHECK-RECOVERY-NEXT: func nullabilityChangeMethod() -> Any?
// CHECK-RECOVERY-NEXT: func typeChangeMethod() -> Any
// CHECK-RECOVERY-NEXT: func disappearingMethodWithOverload()
// CHECK-RECOVERY-NEXT: /* placeholder for disappearingProperty */
// CHECK-RECOVERY-NEXT: var disappearingPropertySetter: Int{{$}}
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class A_Sub2 : A_Sub {
// CHECK-RECOVERY-NEXT: func disappearingMethod()
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class A_Sub3Final : Base {
// CHECK-RECOVERY-NEXT: func disappearingMethod()
// CHECK-RECOVERY-NEXT: func nullabilityChangeMethod() -> Any?
// CHECK-RECOVERY-NEXT: func typeChangeMethod() -> Any
// CHECK-RECOVERY-NEXT: func disappearingMethodWithOverload()
// CHECK-RECOVERY-NEXT: /* placeholder for disappearingProperty */
// CHECK-RECOVERY-NEXT: var disappearingPropertySetter: Int{{$}}
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}

extension Base {
  @nonobjc func disappearingMethodWithOverload() -> SwiftOnlyClass? { return nil }
}

public class B_GenericSub : GenericBase<Base> {
  public override func disappearingMethod() {}
  public override func nullabilityChangeMethod() -> Base? { return nil }
  public override func typeChangeMethod() -> Any { return self }
}

// CHECK-LABEL: class B_GenericSub : GenericBase<Base> {
// CHECK-NEXT: func disappearingMethod()
// CHECK-NEXT: func nullabilityChangeMethod() -> Base?
// CHECK-NEXT: func typeChangeMethod() -> Any
// CHECK-NEXT: init()
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class B_GenericSub : GenericBase<Base> {
// CHECK-RECOVERY-NEXT: func disappearingMethod()
// CHECK-RECOVERY-NEXT: func nullabilityChangeMethod() -> Base?
// CHECK-RECOVERY-NEXT: func typeChangeMethod() -> Any
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}


public class C1_IndexedSubscriptDisappears : IndexedSubscriptDisappearsBase {
  public override subscript(index: Int) -> Any { return self }
}

// CHECK-LABEL: class C1_IndexedSubscriptDisappears : IndexedSubscriptDisappearsBase {
// CHECK-NEXT: subscript(index: Int) -> Any { get }
// CHECK-NEXT: init()
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class C1_IndexedSubscriptDisappears : IndexedSubscriptDisappearsBase {
// CHECK-RECOVERY-NEXT: /* placeholder for subscript(_:) */
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}


public class C2_KeyedSubscriptDisappears : KeyedSubscriptDisappearsBase {
  public override subscript(key: Any) -> Any { return key }
}

// CHECK-LABEL: class C2_KeyedSubscriptDisappears : KeyedSubscriptDisappearsBase {
// CHECK-NEXT: subscript(key: Any) -> Any { get }
// CHECK-NEXT: init()
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class C2_KeyedSubscriptDisappears : KeyedSubscriptDisappearsBase {
// CHECK-RECOVERY-NEXT: /* placeholder for subscript(_:) */
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}


public class C3_GenericIndexedSubscriptDisappears : GenericIndexedSubscriptDisappearsBase<Base> {
  public override subscript(index: Int) -> Base { fatalError() }
}

// CHECK-LABEL: class C3_GenericIndexedSubscriptDisappears : GenericIndexedSubscriptDisappearsBase<Base> {
// CHECK-NEXT: subscript(index: Int) -> Base { get }
// CHECK-NEXT: init()
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class C3_GenericIndexedSubscriptDisappears : GenericIndexedSubscriptDisappearsBase<Base> {
// CHECK-RECOVERY-NEXT: /* placeholder for subscript(_:) */
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}


public class C4_GenericKeyedSubscriptDisappears : GenericKeyedSubscriptDisappearsBase<Base> {
  public override subscript(key: Any) -> Base { fatalError() }
}

// CHECK-LABEL: class C4_GenericKeyedSubscriptDisappears : GenericKeyedSubscriptDisappearsBase<Base> {
// CHECK-NEXT: subscript(key: Any) -> Base { get }
// CHECK-NEXT: init()
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class C4_GenericKeyedSubscriptDisappears : GenericKeyedSubscriptDisappearsBase<Base> {
// CHECK-RECOVERY-NEXT: /* placeholder for subscript(_:) */
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}


open class D1_DesignatedInitDisappears : DesignatedInitDisappearsBase {
  public override init() { fatalError() }
  public override init(value: Int) { fatalError() }
}

// CHECK-LABEL: class D1_DesignatedInitDisappears : DesignatedInitDisappearsBase {
// CHECK-NEXT: init()
// CHECK-NEXT: init(value: Int)
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class D1_DesignatedInitDisappears : DesignatedInitDisappearsBase {
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: /* placeholder for init(value:) */
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}


open class D2_OnlyDesignatedInitDisappears : OnlyDesignatedInitDisappearsBase {
  public override init(value: Int) { fatalError() }
}

// CHECK-LABEL: class D2_OnlyDesignatedInitDisappears : OnlyDesignatedInitDisappearsBase {
// CHECK-NEXT: init(value: Int)
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class D2_OnlyDesignatedInitDisappears : OnlyDesignatedInitDisappearsBase {
// CHECK-RECOVERY-NEXT: /* placeholder for init(value:) */
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}


open class D3_ConvenienceInitDisappears : ConvenienceInitDisappearsBase {
  public override init() { fatalError() }
}

// CHECK-LABEL: class D3_ConvenienceInitDisappears : ConvenienceInitDisappearsBase {
// CHECK-NEXT: init()
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class D3_ConvenienceInitDisappears : ConvenienceInitDisappearsBase {
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}


open class D4_UnknownInitDisappears : UnknownInitDisappearsBase {
  public override init() { fatalError() }
  public override init(value: Int) { fatalError() }
}

// CHECK-LABEL: class D4_UnknownInitDisappears : UnknownInitDisappearsBase {
// CHECK-NEXT: init()
// CHECK-NEXT: init(value: Int)
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class D4_UnknownInitDisappears : UnknownInitDisappearsBase {
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: /* placeholder for init(value:) */
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}


open class D5_OnlyUnknownInitDisappears : OnlyUnknownInitDisappearsBase {
  public override init(value: Int) { fatalError() }
}

// CHECK-LABEL: class D5_OnlyUnknownInitDisappears : OnlyUnknownInitDisappearsBase {
// CHECK-NEXT: init(value: Int)
// CHECK-NEXT: init()
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class D5_OnlyUnknownInitDisappears : OnlyUnknownInitDisappearsBase {
// CHECK-RECOVERY-NEXT: /* placeholder for init(value:) */
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}

open class D6_UnknownInitDisappearsGrandchild : D4_UnknownInitDisappears {
  public override init() { fatalError() }
  public override init(value: Int) { fatalError() }
}

// CHECK-LABEL: class D6_UnknownInitDisappearsGrandchild : D4_UnknownInitDisappears {
// CHECK-NEXT: init()
// CHECK-NEXT: init(value: Int)
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class D6_UnknownInitDisappearsGrandchild : D4_UnknownInitDisappears {
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: /* placeholder for init(value:) */
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}

open class D7_UnknownInitDisappearsGrandchildRequired : D4_UnknownInitDisappears {
  public override init() { fatalError() }
  public required override init(value: Int) { fatalError() }
}

// CHECK-LABEL: class D7_UnknownInitDisappearsGrandchildRequired : D4_UnknownInitDisappears {
// CHECK-NEXT: init()
// CHECK-NEXT: init(value: Int)
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class D7_UnknownInitDisappearsGrandchildRequired : D4_UnknownInitDisappears {
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: /* placeholder for init(value:) */
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}

open class D8_UnknownInitDisappearsRequired : UnknownInitDisappearsBase {
  public override init() { fatalError() }
  public required override init(value: Int) { fatalError() }
}

// CHECK-LABEL: class D8_UnknownInitDisappearsRequired : UnknownInitDisappearsBase {
// CHECK-NEXT: init()
// CHECK-NEXT: init(value: Int)
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class D8_UnknownInitDisappearsRequired : UnknownInitDisappearsBase {
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: /* placeholder for init(value:) */
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}

open class D9_UnknownInitDisappearsRequiredGrandchild : D8_UnknownInitDisappearsRequired {
  public override init() { fatalError() }
  public required init(value: Int) { fatalError() }
}

// CHECK-LABEL: class D9_UnknownInitDisappearsRequiredGrandchild : D8_UnknownInitDisappearsRequired {
// CHECK-NEXT: init()
// CHECK-NEXT: init(value: Int)
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class D9_UnknownInitDisappearsRequiredGrandchild : D8_UnknownInitDisappearsRequired {
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: /* placeholder for init(value:) */
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}

public class E1_MethodWithDisappearingType : MethodWithDisappearingType {
  public override func boxItUp() -> BoxedInt { fatalError() }
}

// CHECK-LABEL: class E1_MethodWithDisappearingType : MethodWithDisappearingType {
// CHECK-NEXT: override func boxItUp() -> BoxedInt
// CHECK-NEXT: init()
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class E1_MethodWithDisappearingType : MethodWithDisappearingType {
// CHECK-RECOVERY-NEXT: /* placeholder for boxItUp() */
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}

public class E2_InitializerStub : InitializerWithDisappearingType {
  public init(unrelatedValue: Int) { fatalError() }
}

// CHECK-LABEL: class E2_InitializerStub : InitializerWithDisappearingType {
// CHECK-NEXT: init(unrelatedValue: Int)
// CHECK-NEXT: init(boxedInt box: BoxedInt)
// CHECK-NEXT: init()
// CHECK-NEXT: deinit
// CHECK-NEXT: {{^}$}}

// CHECK-RECOVERY-LABEL: class E2_InitializerStub : InitializerWithDisappearingType {
// CHECK-RECOVERY-NEXT: init(unrelatedValue: Int)
// CHECK-RECOVERY-NEXT: /* placeholder for init(boxedInt:) */
// CHECK-RECOVERY-NEXT: init()
// CHECK-RECOVERY-NEXT: deinit
// CHECK-RECOVERY-NEXT: {{^}$}}

#endif // TEST
