// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -enable-source-import -emit-module -emit-module-doc -o %t %s -disable-objc-attr-requires-foundation-module
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -parse-as-library %t/cdecl.swiftmodule -typecheck -emit-objc-header-path %t/cdecl.h -import-objc-header %S/../Inputs/empty.h -disable-objc-attr-requires-foundation-module
// RUN: %FileCheck %s < %t/cdecl.h
// RUN: %check-in-clang %t/cdecl.h
// RUN: %check-in-clang -fno-modules -Qunused-arguments %t/cdecl.h -include ctypes.h -include CoreFoundation.h
// RUN: %check-in-clang-cxx -fno-modules -Qunused-arguments %t/cdecl.h -include ctypes.h -include CoreFoundation.h

// REQUIRES: objc_interop

// CHECK: /// What a nightmare!
// CHECK-LABEL: SWIFT_EXTERN double (^ _Nonnull block_nightmare(SWIFT_NOESCAPE float (^ _Nonnull x)(NSInteger)))(char) SWIFT_NOEXCEPT SWIFT_WARN_UNUSED_RESULT;

/// What a nightmare!
@_cdecl("block_nightmare")
public func block_nightmare(x: @convention(block) (Int) -> Float)
  -> @convention(block) (CChar) -> Double { return { _ in 0 } }

// CHECK-LABEL: SWIFT_EXTERN double (^ _Nonnull block_recurring_nightmare(float (^ _Nonnull x)(SWIFT_NOESCAPE NSInteger (^ _Nonnull)(double))))(SWIFT_NOESCAPE char (^ _Nonnull)(unsigned char)) SWIFT_NOEXCEPT SWIFT_WARN_UNUSED_RESULT;
@_cdecl("block_recurring_nightmare")
public func block_recurring_nightmare(x: @escaping @convention(block) (@convention(block) (Double) -> Int) -> Float)
  -> @convention(block) (_ asdfasdf: @convention(block) (CUnsignedChar) -> CChar) -> Double {
  fatalError()
}

// CHECK-LABEL: SWIFT_EXTERN void foo_bar(NSInteger x, NSInteger y) SWIFT_NOEXCEPT;
@_cdecl("foo_bar")
func foo(x: Int, bar y: Int) {}

// CHECK-LABEL: SWIFT_EXTERN double (* _Nonnull function_pointer_nightmare(float (* _Nonnull x)(NSInteger)))(char) SWIFT_NOEXCEPT SWIFT_WARN_UNUSED_RESULT;
@_cdecl("function_pointer_nightmare")
func function_pointer_nightmare(x: @convention(c) (Int) -> Float)
  -> @convention(c) (CChar) -> Double { return { _ in 0 } }

// CHECK-LABEL: SWIFT_EXTERN double (* _Nonnull function_pointer_recurring_nightmare(float (* _Nonnull x)(NSInteger (* _Nonnull)(double))))(char (* _Nonnull)(unsigned char)) SWIFT_NOEXCEPT SWIFT_WARN_UNUSED_RESULT;
@_cdecl("function_pointer_recurring_nightmare")
public func function_pointer_recurring_nightmare(x: @escaping @convention(c) (@convention(c) (Double) -> Int) -> Float)
  -> @convention(c) (@convention(c) (CUnsignedChar) -> CChar) -> Double {
  fatalError()
}
  
// CHECK-LABEL: SWIFT_EXTERN void has_keyword_arg_names(NSInteger auto_, NSInteger union_) SWIFT_NOEXCEPT;
@_cdecl("has_keyword_arg_names")
func keywordArgNames(auto: Int, union: Int) {}

@objc
class C {}

// CHECK-LABEL: SWIFT_EXTERN C * _Null_unspecified return_iuo(void) SWIFT_NOEXCEPT SWIFT_WARN_UNUSED_RESULT;
@_cdecl("return_iuo")
func returnIUO() -> C! { return C() }

// CHECK-LABEL: SWIFT_EXTERN void return_never(void) SWIFT_NOEXCEPT SWIFT_NORETURN;
@_cdecl("return_never")
func returnNever() -> Never { fatalError() }

// CHECK-LABEL: SWIFT_EXTERN void takes_iuo(C * _Null_unspecified c) SWIFT_NOEXCEPT;
@_cdecl("takes_iuo")
func takesIUO(c: C!) {}
