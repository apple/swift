// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource) -typecheck -verify -verify-ignore-unknown -I %S/Inputs -cxx-interoperability-mode=default -Xcc -D_CRT_SECURE_NO_WARNINGS %s
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource) -typecheck -verify -verify-ignore-unknown -I %S/Inputs -Xcc -D_CRT_SECURE_NO_WARNINGS %s

import CustomStringBuiltins

public func testMemcpyOptionalReturn(p: UnsafeMutableRawPointer, e: UnsafeRawPointer, c: UnsafePointer<CChar>, pc: UnsafeMutablePointer<CChar>) {
  // This 'memcpy' is a builtin and is always an optional, regardless of _Nonnull.
  let x = CustomStringBuiltins.memcpy(p, e, 1)!

  // Not a builtin, _Nonnull makes it a non-optional.
  let y = CustomStringBuiltins.memcpy42(p, e, 1)! // expected-error {{cannot force unwrap value of non-optional type 'UnsafeMutableRawPointer'}}

  // other builtins from 'string.h'
  let _ = CustomStringBuiltins.memchr(e, 42, 1)!
  let _ = CustomStringBuiltins.memmove(p, e, 42)!
  let _ = CustomStringBuiltins.memset(p, 1, 42)!
  let _ = CustomStringBuiltins.strrchr(c, 0)!
  let _ = CustomStringBuiltins.strcpy(pc, c)!
  let _ = CustomStringBuiltins.strcat(pc, c)!
}
