// Target-specific tests for C++ copy constructor code generation.

// RUN: %swift %use_no_opaque_pointers -module-name MySwift -target x86_64-unknown-windows-msvc -dump-clang-diagnostics -I %S/Inputs -enable-experimental-cxx-interop -emit-ir %s -parse-stdlib -parse-as-library -disable-legacy-type-info | %FileCheck %s -check-prefix=MICROSOFT_X64
// RUN: %swift -module-name MySwift -target x86_64-unknown-windows-msvc -dump-clang-diagnostics -I %S/Inputs -enable-experimental-cxx-interop -emit-ir %s -parse-stdlib -parse-as-library -disable-legacy-type-info

// REQUIRES: OS=windows-msvc
// REQUIRES: CPU=x86_64

import Constructors
import TypeClassification

// MICROSOFT_X64-LABEL: define dllexport swiftcc void @"$s7MySwift35copyWithUserProvidedCopyConstructorySo03Has{{efgH0V_ADtADF|cdeF0V_ACtACF}}"
// MICROSOFT_X64-SAME: (%TSo30HasUserProvidedCopyConstructorV* {{.*}}[[ARG0:%.*]], %TSo30HasUserProvidedCopyConstructorV* {{.*}}[[ARG1:%.*]], %TSo30HasUserProvidedCopyConstructorV* {{.*}}[[ARG2:%.*]])
// MICROSOFT_X64: [[ARG0_AS_STRUCT:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG0]] to %struct.HasUserProvidedCopyConstructor*
// MICROSOFT_X64: [[ARG2_AS_STRUCT:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG2]] to %struct.HasUserProvidedCopyConstructor*
// MICROSOFT_X64: call %struct.HasUserProvidedCopyConstructor* @"??0HasUserProvidedCopyConstructor@@QEAA@AEBU0@@Z"(%struct.HasUserProvidedCopyConstructor* [[ARG0_AS_STRUCT]], %struct.HasUserProvidedCopyConstructor* [[ARG2_AS_STRUCT]])
// MICROSOFT_X64: [[ARG1_AS_STRUCT:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG1]] to %struct.HasUserProvidedCopyConstructor*
// MICROSOFT_X64: [[ARG2_AS_STRUCT:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG2]] to %struct.HasUserProvidedCopyConstructor*
// MICROSOFT_X64: call %struct.HasUserProvidedCopyConstructor* @"??0HasUserProvidedCopyConstructor@@QEAA@AEBU0@@Z"(%struct.HasUserProvidedCopyConstructor* [[ARG1_AS_STRUCT]], %struct.HasUserProvidedCopyConstructor* [[ARG2_AS_STRUCT]])
// MICROSOFT_X64: ret void

public func copyWithUserProvidedCopyConstructor(_ x: HasUserProvidedCopyConstructor)
  -> (HasUserProvidedCopyConstructor, HasUserProvidedCopyConstructor) {
  return (x, x)
}
