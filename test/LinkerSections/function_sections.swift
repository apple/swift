// REQUIRES: OS=linux-gnu || OS=linux-androideabi || OS=linux-android
// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -function-sections -emit-module -emit-library -static -parse-stdlib %S/Inputs/FunctionSections.swift
// RUN: %target-build-swift -Xlinker --gc-sections -Xlinker -Map=%t/../../FunctionSections.map -I%t/../.. -L%t/../.. -lFunctionSections %S/Inputs/FunctionSectionsUse.swift
// RUN: %FileCheck %s < %t/../../FunctionSections.map

// CHECK: Discarded input sections
// CHECK: .text.$s16FunctionSections5func2yyF
// CHECK: Memory map
// CHECK: .text.$s16FunctionSections5func1yyF
