// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Expose -enable-experimental-cxx-interop -clang-header-expose-decls=has-expose-attr -emit-clang-header-path %t/expose.h
// RUN: %FileCheck %s < %t/expose.h

// RUN: %check-interop-cxx-header-in-clang(%t/expose.h -Wno-error=unused-function)

// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -emit-module -module-name Expose -o %t
// RUN: %target-swift-frontend -parse-as-library %t/Expose.swiftmodule -typecheck -module-name Expose -enable-experimental-cxx-interop -clang-header-expose-decls=has-expose-attr -emit-clang-header-path %t/expose.h
// RUN: %FileCheck %s < %t/expose.h

// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -enable-library-evolution -typecheck -emit-module-interface-path %t/Expose.swiftinterface -module-name Expose
// RUN: %target-swift-frontend -parse-as-library %t/Expose.swiftinterface -enable-library-evolution -disable-objc-attr-requires-foundation-module -typecheck -module-name Expose -enable-experimental-cxx-interop -clang-header-expose-decls=has-expose-attr -emit-clang-header-path %t/expose.h
// RUN: %FileCheck %s < %t/expose.h

// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -enable-library-evolution -typecheck -emit-module-interface-path %t/Expose.swiftinterface -module-name Expose
// RUN: %target-swift-frontend -parse-as-library %t/Expose.swiftinterface -enable-library-evolution -disable-objc-attr-requires-foundation-module -typecheck -module-name Expose -clang-header-expose-decls=has-expose-attr-or-stdlib -emit-clang-header-path %t/expose.h
// RUN: %FileCheck %s < %t/expose.h

@_expose(Cxx)
public func exposed1() {
}

public func exposed2NotExposed() {
}

@_expose(Cxx)
public func exposed3() {
}

@_expose(Cxx, "exposed4")
public func exposed4Renamed() {
}

@_expose(Cxx)
public struct ExposedStruct {
    public var x: Int

    public func method() {}
}

public struct NotExposedStruct {
    public var x: Int
}

@_expose(Cxx, "ExposedStruct2")
public struct ExposedStructRenamed {
    public var y: Int

    @_expose(Cxx)
    public init() { y = 42; prop = 0; prop2 = 0; }

    @_expose(Cxx, "initWithValue")
    public init(why x: Int) { y = x; prop = 0; prop2 = 0; }

    @_expose(Cxx, "renamedProp")
    public var prop: Int

    @_expose(Cxx, "prop3")
    public let prop2: Int

    @_expose(Cxx, "renamedMethod")
    public func method() {}

    public func getNonExposedStruct() -> NotExposedStruct {
        return NotExposedStruct(x: 2)
    }
    // FIXME: if 'getNonExposedStruct' has explicit @_expose we should error in Sema.

    public func passNonExposedStruct(_ x: NotExposedStruct) {
    }
    // FIXME: if 'passNonExposedStruct' has explicit @_expose we should error in Sema.
}

@_expose(Cxx)
public final class ExposedClass {
    public func method() {}
}

// CHECK: class SWIFT_SYMBOL("{{.*}}") ExposedClass final
// CHECK: class SWIFT_SYMBOL("{{.*}}") ExposedStruct final {
// CHECK: class SWIFT_SYMBOL("{{.*}}") ExposedStruct2 final {
// CHECK: ExposedStruct2(ExposedStruct2 &&)
// CHECK: }
// CHECK-NEXT: swift::Int getY() const SWIFT_SYMBOL("{{.*}}");
// CHECK-NEXT: void setY(swift::Int value) SWIFT_SYMBOL("{{.*}}");
// CHECK-NEXT: static SWIFT_INLINE_THUNK ExposedStruct2 init() SWIFT_SYMBOL("{{.*}}");
// CHECK-NEXT: static SWIFT_INLINE_THUNK ExposedStruct2 initWithValue(swift::Int x) SWIFT_SYMBOL("{{.*}}");
// CHECK-NEXT: swift::Int getRenamedProp() const SWIFT_SYMBOL("{{.*}}");
// CHECK-NEXT: void setRenamedProp(swift::Int value) SWIFT_SYMBOL("{{.*}}");
// CHECK-NEXT: swift::Int getProp3() const SWIFT_SYMBOL("{{.*}}");
// CHECK-NEXT: void renamedMethod() const SWIFT_SYMBOL("{{.*}}");
// CHECK-NEXT: private:

// CHECK: SWIFT_INLINE_THUNK void exposed1() noexcept SWIFT_SYMBOL("{{.*}}") {
// CHECK-NEXT:   return _impl::$s6Expose8exposed1yyF();
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-EMPTY:
// CHECK-NEXT: SWIFT_INLINE_THUNK void exposed3() noexcept SWIFT_SYMBOL("{{.*}}") {
// CHECK-NEXT:   return _impl::$s6Expose8exposed3yyF();
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-EMPTY:
// CHECK-NEXT: SWIFT_INLINE_THUNK void exposed4() noexcept SWIFT_SYMBOL("{{.*}}") {
// CHECK-NEXT:   return _impl::$s6Expose15exposed4RenamedyyF();
// CHECK-NEXT: }

// CHECK: void ExposedClass::method()
// CHECK: swift::Int ExposedStruct::getX() const {
// CHECK: void ExposedStruct::setX(swift::Int value) {
// CHECK: void ExposedStruct::method() const {
// CHECK: swift::Int ExposedStruct2::getY() const {
// CHECK: void ExposedStruct2::setY(swift::Int value) {
// CHECK: ExposedStruct2 ExposedStruct2::init() {
// CHECK: ExposedStruct2 ExposedStruct2::initWithValue(swift::Int x) {
// CHECK: swift::Int ExposedStruct2::getRenamedProp() const {
// CHECK: void ExposedStruct2::setRenamedProp(swift::Int value) {
// CHECK: swift::Int ExposedStruct2::getProp3() const {
// CHECK: void ExposedStruct2::renamedMethod() const {

// CHECK-NOT: NonExposedStruct

