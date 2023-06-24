// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) %use_no_opaque_pointers -I %S/Inputs -enable-experimental-cxx-interop -emit-ir %s -Xcc -fignore-exceptions | %FileCheck %s
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -I %S/Inputs -enable-experimental-cxx-interop -emit-ir %s -Xcc -fignore-exceptions

// REQUIRES: CPU=x86_64
// REQUIRES: objc_interop
import Foundation
import ConstructorsObjC

public func createConstructorWithNSArrayParam() -> ConstructorWithNSArrayParam {
  // CHECK: define swiftcc void @"$s4main33createConstructorWithNSArrayParamSo0cdeF0VyF"()
  // CHECK-NOT: define
  // CHECK: [[VAR:%[0-9]+]] = alloca %TSo27ConstructorWithNSArrayParamV, align 1
  // CHECK: %{{[0-9]+}} = call swiftcc %TSo7NSArrayC* @"$sSa10FoundationE19_bridgeToObjectiveCSo7NSArrayCyF"(%swift.bridge* %{{[0-9]+}}, %swift.type* getelementptr inbounds (%swift.full_existential_type, %swift.full_existential_type* @"$sypN", i32 0, i32 1))
  // CHECK: [[CAST_VAR:%[0-9]+]] = bitcast %TSo27ConstructorWithNSArrayParamV* [[VAR]] to %struct.ConstructorWithNSArrayParam*
  // CHECK: call void @_ZN27ConstructorWithNSArrayParamC1EP7NSArray(%struct.ConstructorWithNSArrayParam* [[CAST_VAR]], [[VAR]]* %{{[0-9]+}})
  return ConstructorWithNSArrayParam([])
}
