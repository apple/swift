// Target-specific tests for C++ copy constructor code generation.

// REQUIRES: CODEGENERATOR=ARM && CODEGENERATOR=X86

// RUN: %swift -module-name Swift -target x86_64-apple-macosx10.9 -dump-clang-diagnostics -I %S/Inputs -enable-cxx-interop -emit-ir %s -parse-stdlib -parse-as-library -disable-legacy-type-info | %FileCheck %s -check-prefix=ITANIUM_X64
// RUN: %swift -module-name Swift -target armv7-none-linux-androideabi -dump-clang-diagnostics -I %S/Inputs -enable-cxx-interop -emit-ir %s -parse-stdlib -parse-as-library -disable-legacy-type-info | %FileCheck %s -check-prefix=ITANIUM_ARM
// RUN: %swift -module-name Swift -target x86_64-unknown-windows-msvc -dump-clang-diagnostics -I %S/Inputs -enable-cxx-interop -emit-ir %s -parse-stdlib -parse-as-library -disable-legacy-type-info | %FileCheck %s -check-prefix=MICROSOFT_X64

import Constructors
import TypeClassification

typealias Void = ()
struct UnsafePointer<T> { }
struct UnsafeMutablePointer<T> { }

// ITANIUM_X64-LABEL: define swiftcc void @"$ss35copyWithUserProvidedCopyConstructorySo03HascdeF0V_ACtACF"
// ITANIUM_X64-SAME: (%TSo30HasUserProvidedCopyConstructorV* {{.*}}[[ARG0:%.*]], %TSo30HasUserProvidedCopyConstructorV* {{.*}}[[ARG1:%.*]], %TSo30HasUserProvidedCopyConstructorV* {{.*}}[[ARG2:%.*]])
// ITANIUM_X64: [[ARG0_AS_STRUCT:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG0]] to %struct.HasUserProvidedCopyConstructor*
// ITANIUM_X64: [[ARG2_AS_STRUCT:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG2]] to %struct.HasUserProvidedCopyConstructor*
// ITANIUM_X64: call void @_ZN30HasUserProvidedCopyConstructorC1ERKS_(%struct.HasUserProvidedCopyConstructor* [[ARG0_AS_STRUCT]], %struct.HasUserProvidedCopyConstructor* [[ARG2_AS_STRUCT]])
// ITANIUM_X64: [[ARG1_AS_STRUCT:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG1]] to %struct.HasUserProvidedCopyConstructor*
// ITANIUM_X64: [[ARG2_AS_STRUCT:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG2]] to %struct.HasUserProvidedCopyConstructor*
// ITANIUM_X64: call void @_ZN30HasUserProvidedCopyConstructorC1ERKS_(%struct.HasUserProvidedCopyConstructor* [[ARG1_AS_STRUCT]], %struct.HasUserProvidedCopyConstructor* [[ARG2_AS_STRUCT]])
// ITANIUM_X64: ret void

// ITANIUM_ARM-LABEL: define protected swiftcc void @"$ss35copyWithUserProvidedCopyConstructorySo03HascdeF0V_ACtACF"
// ITANIUM_ARM-SAME: (%TSo30HasUserProvidedCopyConstructorV* {{.*}}[[ARG0:%.*]], %TSo30HasUserProvidedCopyConstructorV* {{.*}}[[ARG1:%.*]], %TSo30HasUserProvidedCopyConstructorV* {{.*}}[[ARG2:%.*]])
// ITANIUM_ARM: [[ARG0_AS_STRUCT:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG0]] to %struct.HasUserProvidedCopyConstructor*
// ITANIUM_ARM: [[ARG2_AS_STRUCT:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG2]] to %struct.HasUserProvidedCopyConstructor*
// ITANIUM_ARM: call %struct.HasUserProvidedCopyConstructor* @_ZN30HasUserProvidedCopyConstructorC2ERKS_(%struct.HasUserProvidedCopyConstructor* [[ARG0_AS_STRUCT]], %struct.HasUserProvidedCopyConstructor* [[ARG2_AS_STRUCT]])
// ITANIUM_ARM: [[ARG1_AS_STRUCT:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG1]] to %struct.HasUserProvidedCopyConstructor*
// ITANIUM_ARM: [[ARG2_AS_STRUCT:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG2]] to %struct.HasUserProvidedCopyConstructor*
// ITANIUM_ARM: call %struct.HasUserProvidedCopyConstructor* @_ZN30HasUserProvidedCopyConstructorC2ERKS_(%struct.HasUserProvidedCopyConstructor* [[ARG1_AS_STRUCT]], %struct.HasUserProvidedCopyConstructor* [[ARG2_AS_STRUCT]])
// ITANIUM_ARM: ret void

// MICROSOFT_X64-LABEL: define dllexport swiftcc void @"$ss35copyWithUserProvidedCopyConstructorySo03HascdeF0V_ACtACF"
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
