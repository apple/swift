// RUN: %empty-directory(%t)
// RUN: split-file %S/bridge-cxx-struct-back-to-cxx.swift %t

// RUN: %target-swift-frontend -typecheck %t/use-cxx-types.swift -typecheck -module-name UseCxxTy -emit-clang-header-path %t/UseCxxTy.h -I %t -enable-experimental-cxx-interop -clang-header-expose-decls=all-public -disable-availability-checking
// RUN: echo "#include \"header.h\"" > %t/full-cxx-swift-cxx-bridging.h
// RUN: cat %t/UseCxxTy.h >> %t/full-cxx-swift-cxx-bridging.h

// Check that the generated header can be
// built with Clang modules enabled in ObjC++.

// REQUIRES: objc_interop

// RUN: %target-interop-build-clangxx -fsyntax-only -x objective-c++-header %t/full-cxx-swift-cxx-bridging.h -std=gnu++20 -c -fmodules -fcxx-modules -I %t
