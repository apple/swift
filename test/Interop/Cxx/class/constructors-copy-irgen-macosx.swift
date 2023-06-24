// Target-specific tests for C++ copy constructor code generation.

// RUN: %swift %use_no_opaque_pointers -module-name MySwift -target x86_64-apple-macosx10.9 -dump-clang-diagnostics -I %S/Inputs -enable-experimental-cxx-interop -emit-ir %s -parse-stdlib -parse-as-library -disable-legacy-type-info -Xcc -fignore-exceptions | %FileCheck %s -check-prefix=ITANIUM_X64
// RUN: %swift -module-name MySwift -target x86_64-apple-macosx10.9 -dump-clang-diagnostics -I %S/Inputs -enable-experimental-cxx-interop -emit-ir %s -parse-stdlib -parse-as-library -disable-legacy-type-info -Xcc -fignore-exceptions

// REQUIRES: OS=macosx
// REQUIRES: CPU=x86_64


import Constructors
import TypeClassification

// ITANIUM_X64-LABEL: define swiftcc void @"$s7MySwift35copyWithUserProvidedCopyConstructorySo03Has{{cdeF0V_ACtACF|efgH0V_ADtADF}}"
// ITANIUM_X64-SAME: (%TSo30HasUserProvidedCopyConstructorV* {{.*}}[[ARG0:%.*]], %TSo30HasUserProvidedCopyConstructorV* {{.*}}[[ARG1:%.*]], %TSo30HasUserProvidedCopyConstructorV* {{.*}}[[ARG2:%.*]])
// ITANIUM_X64: [[ARG0_AS_STRUCT:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG0]] to %struct.HasUserProvidedCopyConstructor*
// ITANIUM_X64: [[ARG2_AS_STRUCT:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG2]] to %struct.HasUserProvidedCopyConstructor*
// ITANIUM_X64: call void @_ZN30HasUserProvidedCopyConstructorC1ERKS_(%struct.HasUserProvidedCopyConstructor* [[ARG0_AS_STRUCT]], %struct.HasUserProvidedCopyConstructor* [[ARG2_AS_STRUCT]])
// ITANIUM_X64: [[ARG1_AS_STRUCT:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG1]] to %struct.HasUserProvidedCopyConstructor*
// ITANIUM_X64: [[ARG2_AS_STRUCT:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG2]] to %struct.HasUserProvidedCopyConstructor*
// ITANIUM_X64: call void @_ZN30HasUserProvidedCopyConstructorC1ERKS_(%struct.HasUserProvidedCopyConstructor* [[ARG1_AS_STRUCT]], %struct.HasUserProvidedCopyConstructor* [[ARG2_AS_STRUCT]])
// ITANIUM_X64: ret void

public func copyWithUserProvidedCopyConstructor(_ x: HasUserProvidedCopyConstructor)
  -> (HasUserProvidedCopyConstructor, HasUserProvidedCopyConstructor) {
  return (x, x)
}
