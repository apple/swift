// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Properties -clang-header-expose-decls=all-public -emit-clang-header-path %t/properties.h
// RUN: %FileCheck %s < %t/properties.h

// RUN: %check-interop-cxx-header-in-clang(%t/properties.h)

public struct IsHasProperties {
    public var isEmpty: Bool { return true }

    public let hasFlavor: Bool = false

    public var isSolid: Bool = true

    public var flag: Bool = false

    public let has: Bool = true

    public let isOption: Int = 0

    // make it a large struct for easier ABI matching.
    let x1, x2, x3, x4, x5, x6: Int
}

// CHECK: class SWIFT_SYMBOL({{.*}}) IsHasProperties

// CHECK: SWIFT_INLINE_THUNK bool isEmpty() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK bool hasFlavor() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK bool isSolid() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK void setIsSolid(bool value) SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK bool getFlag() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK void setFlag(bool value) SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK bool getHas() const SWIFT_SYMBOL({{.*}});
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int getIsOption() const SWIFT_SYMBOL({{.*}});

// CHECK: SWIFT_INLINE_THUNK bool IsHasProperties::isEmpty() const {
// CHECK-NEXT: return _impl::$s10Properties05IsHasA0V7isEmptySbvg(_getOpaquePointer());
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK bool IsHasProperties::hasFlavor() const {
// CHECK-NEXT: return _impl::$s10Properties05IsHasA0V9hasFlavorSbvg(_getOpaquePointer());
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK bool IsHasProperties::isSolid() const {
// CHECK-NEXT: return _impl::$s10Properties05IsHasA0V7isSolidSbvg(_getOpaquePointer());
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK void IsHasProperties::setIsSolid(bool value) {
// CHECK-NEXT: _impl::$s10Properties05IsHasA0V7isSolidSbvs(value, _getOpaquePointer());
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK bool IsHasProperties::getFlag() const {
// CHECK-NEXT: return _impl::$s10Properties05IsHasA0V4flagSbvg(_getOpaquePointer());
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK void IsHasProperties::setFlag(bool value) {
// CHECK-NEXT: _impl::$s10Properties05IsHasA0V4flagSbvs(value, _getOpaquePointer());
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK bool IsHasProperties::getHas() const {
// CHECK-NEXT: return _impl::$s10Properties05IsHasA0V3hasSbvg(_getOpaquePointer());
// CHECK-NEXT: }
// CHECK-NEXT: SWIFT_INLINE_THUNK swift::Int IsHasProperties::getIsOption() const {
// CHECK-NEXT: return _impl::$s10Properties05IsHasA0V8isOptionSivg(_getOpaquePointer());
// CHECK-NEXT: }
