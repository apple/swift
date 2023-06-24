// RUN: %target-swift-frontend %use_no_opaque_pointers -enable-experimental-cxx-interop -I %S/Inputs %s -emit-ir -Xcc -fignore-exceptions | %FileCheck %s
// RUN: %target-swift-frontend -enable-experimental-cxx-interop -I %S/Inputs %s -emit-ir -Xcc -fignore-exceptions

// REQUIRES: CPU=x86_64
// REQUIRES: objc_interop

import CopyConstructors

// CHECK-LABEL: define swiftcc void @"$s4main31testUserProvidedCopyConstructor3objSo03HascdeF0V_AEtAE_tF"
// CHECK: [[T0_DEST:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG0:%[0-9]+]] to %struct.HasUserProvidedCopyConstructor*
// CHECK: [[T0_SRC:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG2:%[0-9]+]] to %struct.HasUserProvidedCopyConstructor*
// CHECK: call void @_ZN30HasUserProvidedCopyConstructorC1ERKS_(%struct.HasUserProvidedCopyConstructor* [[T0_DEST]], %struct.HasUserProvidedCopyConstructor* [[T0_SRC]])
// CHECK: [[T1_DEST:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG1:%[0-9]+]] to %struct.HasUserProvidedCopyConstructor*
// CHECK: [[T1_SRC:%.*]] = bitcast %TSo30HasUserProvidedCopyConstructorV* [[ARG2]] to %struct.HasUserProvidedCopyConstructor*
// CHECK: call void @_ZN30HasUserProvidedCopyConstructorC1ERKS_(%struct.HasUserProvidedCopyConstructor* [[T1_DEST]], %struct.HasUserProvidedCopyConstructor* [[T1_SRC]])
// CHECK: ret void

// CHECK-LABEL: define linkonce_odr void @_ZN30HasUserProvidedCopyConstructorC1ERKS_

public func testUserProvidedCopyConstructor(obj : HasUserProvidedCopyConstructor) -> (HasUserProvidedCopyConstructor, HasUserProvidedCopyConstructor) {
  return (obj, obj)
}

// CHECK-LABEL: define swiftcc void @"$s4main26testDefaultCopyConstructor3defSo013HasNonTrivialcdE0V_AEtAE_tF"
// CHECK: call void @_ZN35HasNonTrivialDefaultCopyConstructorC1ERKS_
// CHECK: call void @_ZN35HasNonTrivialDefaultCopyConstructorC1ERKS_

// Make sure we call the copy constructor of our member (HasUserProvidedCopyConstructor)
// CHECK-LABEL: define linkonce_odr void @_ZN35HasNonTrivialDefaultCopyConstructorC1ERKS_
// CHECK: call void @_ZN35HasNonTrivialDefaultCopyConstructorC2ERKS_

public func testDefaultCopyConstructor(def : HasNonTrivialDefaultCopyConstructor) -> (HasNonTrivialDefaultCopyConstructor, HasNonTrivialDefaultCopyConstructor) {
  return (def, def)
}

// CHECK-LABEL: define swiftcc void @"$s4main27testImplicitCopyConstructor3impSo013HasNonTrivialcdE0V_AEtAE_tF"
// CHECK: call void @_ZN36HasNonTrivialImplicitCopyConstructorC1ERKS_
// CHECK: call void @_ZN36HasNonTrivialImplicitCopyConstructorC1ERKS_

// Same as above.
// CHECK-LABEL: define linkonce_odr void @_ZN36HasNonTrivialImplicitCopyConstructorC1ERKS_
// CHECK: call void  @_ZN36HasNonTrivialImplicitCopyConstructorC2ERKS_

public func testImplicitCopyConstructor(imp : HasNonTrivialImplicitCopyConstructor) -> (HasNonTrivialImplicitCopyConstructor, HasNonTrivialImplicitCopyConstructor) {
  return (imp, imp)
}
