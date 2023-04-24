// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module-path %t/Library.swiftmodule -I %S/Inputs/implementation-only-override -enable-library-evolution -enable-objc-interop -module-name Library %s
// RUN: %target-swift-ide-test -print-module -module-to-print=Library -I %t -I %S/Inputs/implementation-only-override -source-filename=x -access-filter-public -enable-objc-interop > %t/Library.txt
// RUN: %FileCheck %s < %t/Library.txt

// CHECK: import FooKit
// CHECK-NOT: import FooKit_SECRET

import FooKit
@_implementationOnly import FooKit_SECRET

// CHECK-LABEL: class GoodChild : Parent {
//  CHECK-NEXT:   override init()
//  CHECK-NEXT:   /* placeholder for init(SECRET:) */
//  CHECK-NEXT:   /* placeholder for init(requiredSECRET:) */
//  CHECK-NEXT:   @_implementationOnly func methodSECRET()
//  CHECK-NEXT:   /* placeholder for roPropSECRET */
//  CHECK-NEXT:   /* placeholder for rwPropSECRET */
//  CHECK-NEXT:   /* placeholder for subscript(_:) */
//  CHECK-NEXT:   @_implementationOnly override var redefinedPropSECRET: Parent?
//  CHECK-NEXT:   deinit
//  CHECK-NEXT: }
public class GoodChild: Parent {
  public override init() { super.init() }
  // FIXME: @_implementationOnly on an initializer doesn't exactly make sense,
  // since they're not inherited.
  @_implementationOnly public override init(SECRET: Int32) { super.init() }
  @_implementationOnly public required init(requiredSECRET: Int32) { super.init() }

  @_implementationOnly public override func methodSECRET() {}
  @_implementationOnly public override var roPropSECRET: Parent? { nil }
  @_implementationOnly public override var rwPropSECRET: Parent? {
    get { nil }
    set {}
  }
  @_implementationOnly public override subscript(_ index: Int32) -> Parent? { nil }
  @_implementationOnly public override var redefinedPropSECRET: Parent? {
    get { nil }
    set {}
  }
}

// CHECK-LABEL: class GoodGenericChild<Toy> : Parent {
//  CHECK-NEXT:   override init()
//  CHECK-NEXT:   /* placeholder for init(SECRET:) */
//  CHECK-NEXT:   /* placeholder for init(requiredSECRET:) */
//  CHECK-NEXT:   @_implementationOnly func methodSECRET()
//  CHECK-NEXT:   /* placeholder for roPropSECRET */
//  CHECK-NEXT:   /* placeholder for rwPropSECRET */
//  CHECK-NEXT:   /* placeholder for subscript(_:) */
//  CHECK-NEXT:   @_implementationOnly override var redefinedPropSECRET: Parent?
//  CHECK-NEXT:   deinit
//  CHECK-NEXT: }
public class GoodGenericChild<Toy>: Parent {
  public override init() { super.init() }
  // FIXME: @_implementationOnly on an initializer doesn't exactly make sense,
  // since they're not inherited.
  @_implementationOnly public override init(SECRET: Int32) { super.init() }
  @_implementationOnly public required init(requiredSECRET: Int32) { super.init() }

  @_implementationOnly public override func methodSECRET() {}
  @_implementationOnly public override var roPropSECRET: Parent? { nil }
  @_implementationOnly public override var rwPropSECRET: Parent? {
    get { nil }
    set {}
  }
  @_implementationOnly public override subscript(_ index: Int32) -> Parent? { nil }
  @_implementationOnly public override var redefinedPropSECRET: Parent? {
    get { nil }
    set {}
  }
}

// CHECK-LABEL: class QuietChild : Parent {
//  CHECK-NEXT:   /* placeholder for init(SECRET:) */
//  CHECK-NEXT:   /* placeholder for init(requiredSECRET:) */
//  CHECK-NEXT:   deinit
//  CHECK-NEXT: }
public class QuietChild: Parent {
  internal override init() { super.init() }
  internal override init(SECRET: Int32) { super.init() }
  internal required init(requiredSECRET: Int32) { super.init() }
}

internal class PrivateChild: Parent {
  override func methodSECRET() {}
  override var roPropSECRET: Parent? { nil }
  override var rwPropSECRET: Parent? {
    get { nil }
    set {}
  }
  override subscript(_ index: Int32) -> Parent? { nil }
  override var redefinedPropSECRET: Parent? {
    get { nil }
    set {}
  }
}

internal class PrivateGrandchild: GoodChild {
  override func methodSECRET() {}
  override var roPropSECRET: Parent? { nil }
  override var rwPropSECRET: Parent? {
    get { nil }
    set {}
  }
  override subscript(_ index: Int32) -> Parent? { nil }
  override var redefinedPropSECRET: Parent? {
    get { nil }
    set {}
  }
}

// CHECK-LABEL: class SubscriptChild : SubscriptParent {
//  CHECK-NEXT:   @_implementationOnly override subscript(index: Int32) -> Parent?
//  CHECK-NEXT:   deinit
//  CHECK-NEXT: }
public class SubscriptChild: SubscriptParent {
  @_implementationOnly public override subscript(_ index: Int32) -> Parent? {
    get { nil }
    set {}
  }
}
