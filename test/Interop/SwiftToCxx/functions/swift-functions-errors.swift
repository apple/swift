// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -typecheck -module-name Functions -enable-experimental-cxx-interop -clang-header-expose-decls=has-expose-attr-or-stdlib -enable-experimental-feature GenerateBindingsForThrowingFunctionsInCXX -emit-clang-header-path %t/functions.h
// RUN: %FileCheck %s < %t/functions.h

// RUN: %check-interop-cxx-header-in-clang(%t/functions.h -DSWIFT_CXX_INTEROP_HIDE_STL_OVERLAY -DSWIFT_CXX_INTEROP_EXPERIMENTAL_SWIFT_ERROR -Wno-unused-function)

// for experimental feature GenerateBindingsForThrowingFunctionsInCXX:
// REQUIRES: asserts

// CHECK-LABEL: namespace Functions SWIFT_PRIVATE_ATTR SWIFT_SYMBOL_MODULE("Functions") {

// CHECK-LABEL: namespace _impl {

// CHECK: SWIFT_EXTERN void $s9Functions18emptyThrowFunctionyyKF(SWIFT_CONTEXT void * _Nonnull _ctx, SWIFT_ERROR_RESULT void * _Nullable * _Nullable _error) SWIFT_CALL; // emptyThrowFunction()
// CHECK: SWIFT_EXTERN void $s9Functions18testDestroyedErroryyKF(SWIFT_CONTEXT void * _Nonnull _ctx, SWIFT_ERROR_RESULT void * _Nullable * _Nullable _error) SWIFT_CALL; // testDestroyedError()
// CHECK: SWIFT_EXTERN void $s9Functions13throwFunctionyyKF(SWIFT_CONTEXT void * _Nonnull _ctx, SWIFT_ERROR_RESULT void * _Nullable * _Nullable _error) SWIFT_CALL; // throwFunction()
// CHECK: SWIFT_EXTERN void $s9Functions28throwFunctionWithNeverReturns0E0OyKF(SWIFT_CONTEXT void * _Nonnull _ctx, SWIFT_ERROR_RESULT void * _Nullable * _Nullable _error) SWIFT_CALL; // throwFunctionWithNeverReturn()
// CHECK: SWIFT_EXTERN ptrdiff_t $s9Functions31throwFunctionWithPossibleReturnyS2iKF(ptrdiff_t a, SWIFT_CONTEXT void * _Nonnull _ctx, SWIFT_ERROR_RESULT void * _Nullable * _Nullable _error) SWIFT_CALL; // throwFunctionWithPossibleReturn(_:)
// CHECK: SWIFT_EXTERN ptrdiff_t $s9Functions23throwFunctionWithReturnSiyKF(SWIFT_CONTEXT void * _Nonnull _ctx, SWIFT_ERROR_RESULT void * _Nullable * _Nullable _error) SWIFT_CALL; // throwFunctionWithReturn()


// CHECK: }

@_expose(Cxx)
public enum NaiveErrors : Error {
    case returnError
    case throwError

    public func getMessage() {
        print(self)
    }
}

@_expose(Cxx)
public func emptyThrowFunction() throws { print("passEmptyThrowFunction") }

// CHECK: SWIFT_INLINE_THUNK swift::ThrowingResult<void> emptyThrowFunction() SWIFT_SYMBOL("s:9Functions18emptyThrowFunctionyyKF") {
// CHECK: void* opaqueError = nullptr;
// CHECK: void* _ctx = nullptr;
// CHECK: _impl::$s9Functions18emptyThrowFunctionyyKF(_ctx, &opaqueError);
// CHECK: if (opaqueError != nullptr)
// CHECK: #ifdef __cpp_exceptions
// CHECK: throw (swift::Error(opaqueError));
// CHECK: #else
// CHECK: return swift::Expected<void>(swift::Error(opaqueError));
// CHECK: #endif
// CHECK: }

class TestDestroyed {
  deinit {
    print("Test destroyed")
  }
}

@_expose(Cxx)
public struct DestroyedError : Error {
  let t = TestDestroyed()
}

@_expose(Cxx)
public func testDestroyedError() throws { throw DestroyedError() }

// CHECK: SWIFT_INLINE_THUNK swift::ThrowingResult<void> testDestroyedError() SWIFT_SYMBOL("s:9Functions18testDestroyedErroryyKF") {
// CHECK: void* opaqueError = nullptr;
// CHECK: void* _ctx = nullptr;
// CHECK: _impl::$s9Functions18testDestroyedErroryyKF(_ctx, &opaqueError);
// CHECK: if (opaqueError != nullptr)
// CHECK: #ifdef __cpp_exceptions
// CHECK: throw (swift::Error(opaqueError));
// CHECK: #else
// CHECK: return swift::Expected<void>(swift::Error(opaqueError));
// CHECK: #endif
// CHECK: }

@_expose(Cxx)
public func throwFunction() throws {
    print("passThrowFunction")
    throw NaiveErrors.throwError
}

// CHECK: SWIFT_INLINE_THUNK swift::ThrowingResult<void> throwFunction() SWIFT_SYMBOL("s:9Functions13throwFunctionyyKF") {
// CHECK: void* opaqueError = nullptr;
// CHECK: void* _ctx = nullptr;
// CHECK: _impl::$s9Functions13throwFunctionyyKF(_ctx, &opaqueError);
// CHECK: if (opaqueError != nullptr)
// CHECK: #ifdef __cpp_exceptions
// CHECK: throw (swift::Error(opaqueError));
// CHECK: #else
// CHECK: return swift::Expected<void>(swift::Error(opaqueError));
// CHECK: #endif
// CHECK: }

@_expose(Cxx)
public func throwFunctionWithNeverReturn() throws -> Never {
    print("passThrowFunctionWithNeverReturn")
    throw NaiveErrors.returnError
}

// CHECK: SWIFT_INLINE_THUNK swift::ThrowingResult<void> throwFunctionWithNeverReturn() SWIFT_SYMBOL("s:9Functions28throwFunctionWithNeverReturns0E0OyKF") SWIFT_NORETURN_EXCEPT_ERRORS {
// CHECK-NEXT: void* opaqueError = nullptr;
// CHECK-NEXT: void* _ctx = nullptr;
// CHECK-NEXT: _impl::$s9Functions28throwFunctionWithNeverReturns0E0OyKF(_ctx, &opaqueError);
// CHECK-NEXT: if (opaqueError != nullptr)
// CHECK-NEXT: #ifdef __cpp_exceptions
// CHECK-NEXT: throw (swift::Error(opaqueError));
// CHECK-NEXT: #else
// CHECK-NEXT: return swift::Expected<void>(swift::Error(opaqueError));
// CHECK-NEXT: #endif
// CHECK-NEXT: abort();
// CHECK-NEXT: }

@_expose(Cxx)
public func throwFunctionWithPossibleReturn(_ a: Int) throws -> Int {
    print("passThrowFunctionWithPossibleReturn")
    if (a == 0) {
        throw NaiveErrors.returnError
    }
    return 0
}

// CHECK: SWIFT_INLINE_THUNK swift::ThrowingResult<swift::Int> throwFunctionWithPossibleReturn(swift::Int a) SWIFT_SYMBOL("s:9Functions31throwFunctionWithPossibleReturnyS2iKF") SWIFT_WARN_UNUSED_RESULT {
// CHECK: void* opaqueError = nullptr;
// CHECK: void* _ctx = nullptr;
// CHECK: auto returnValue = _impl::$s9Functions31throwFunctionWithPossibleReturnyS2iKF(a, _ctx, &opaqueError);
// CHECK: if (opaqueError != nullptr)
// CHECK: #ifdef __cpp_exceptions
// CHECK: throw (swift::Error(opaqueError));
// CHECK: #else
// CHECK: return swift::Expected<swift::Int>(swift::Error(opaqueError));
// CHECK: #endif
// CHECK: return SWIFT_RETURN_THUNK(swift::Int, returnValue);
// CHECK: }

@_expose(Cxx)
public func throwFunctionWithReturn() throws -> Int {
    print("passThrowFunctionWithReturn")
    throw NaiveErrors.returnError
    return 0
}

// CHECK: SWIFT_INLINE_THUNK swift::ThrowingResult<swift::Int> throwFunctionWithReturn() SWIFT_SYMBOL("s:9Functions23throwFunctionWithReturnSiyKF") SWIFT_WARN_UNUSED_RESULT {
// CHECK: void* opaqueError = nullptr;
// CHECK: void* _ctx = nullptr;
// CHECK: auto returnValue = _impl::$s9Functions23throwFunctionWithReturnSiyKF(_ctx, &opaqueError);
// CHECK: #ifdef __cpp_exceptions
// CHECK: throw (swift::Error(opaqueError));
// CHECK: #else
// CHECK: return swift::Expected<swift::Int>(swift::Error(opaqueError));
// CHECK: #endif
// CHECK: return SWIFT_RETURN_THUNK(swift::Int, returnValue);
// CHECK: }
