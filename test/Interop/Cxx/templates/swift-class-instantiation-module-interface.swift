// RUN: %empty-directory(%t)
// RUN: %target-swiftxx-frontend -emit-module -o %t/SwiftClassTemplateModule.swiftmodule %S/Inputs/SwiftClassInstantiationModule.swift -I %S/Inputs -swift-version 5 -disable-availability-checking
// RUN: %target-swift-ide-test -print-module -module-to-print=SwiftClassTemplateModule -I %t/ -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s


// CHECK: import ClassTemplateForSwiftModule
// CHECK: func makeWrappedMagicNumber() -> MagicWrapper<IntWrapper>
// CHECK: func readWrappedMagicNumber(_ i: inout MagicWrapper<IntWrapper>) -> CInt
