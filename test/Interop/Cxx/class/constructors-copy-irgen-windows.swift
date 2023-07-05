// Target-specific tests for C++ copy constructor code generation.

// RUN: %swift -module-name MySwift -target x86_64-unknown-windows-msvc -dump-clang-diagnostics -I %S/Inputs -enable-experimental-cxx-interop -emit-ir %s -parse-stdlib -parse-as-library -disable-legacy-type-info | %FileCheck %s -check-prefix=MICROSOFT_X64

// REQUIRES: OS=windows-msvc
// REQUIRES: CPU=x86_64

import Constructors
import TypeClassification

// MICROSOFT_X64-LABEL: define dllexport swiftcc void @"$s7MySwift35copyWithUserProvidedCopyConstructorySo03Has{{efgH0V_ADtADF|cdeF0V_ACtACF}}"
// MICROSOFT_X64-SAME: (ptr {{.*}}[[ARG0:%.*]], ptr {{.*}}[[ARG1:%.*]], ptr {{.*}}[[ARG2:%.*]])
// MICROSOFT_X64: call ptr @"??0HasUserProvidedCopyConstructor@@QEAA@AEBU0@@Z"(ptr [[ARG0]], ptr [[ARG2]])
// MICROSOFT_X64: call ptr @"??0HasUserProvidedCopyConstructor@@QEAA@AEBU0@@Z"(ptr [[ARG1]], ptr [[ARG2]])
// MICROSOFT_X64: ret void

public func copyWithUserProvidedCopyConstructor(_ x: HasUserProvidedCopyConstructor)
  -> (HasUserProvidedCopyConstructor, HasUserProvidedCopyConstructor) {
  return (x, x)
}
