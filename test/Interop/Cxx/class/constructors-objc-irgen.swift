// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -I %S/Inputs -enable-experimental-cxx-interop -emit-ir %s -Xcc -fignore-exceptions | %FileCheck %s

// REQUIRES: CPU=x86_64
// REQUIRES: objc_interop
import Foundation
import ConstructorsObjC

public func createConstructorWithNSArrayParam() -> ConstructorWithNSArrayParam {
  // CHECK: define swiftcc void @"$s4main33createConstructorWithNSArrayParamSo0cdeF0VyF"()
  // CHECK-NOT: define
  // CHECK: [[VAR:%[0-9]+]] = alloca %TSo27ConstructorWithNSArrayParamV, align 1
  // CHECK: %{{[0-9]+}} = call swiftcc ptr @"$sSa10FoundationE19_bridgeToObjectiveCSo7NSArrayCyF"(ptr %{{[0-9]+}}, ptr getelementptr inbounds (%swift.full_existential_type, ptr @"$sypN", i32 0, i32 1))
  // CHECK: call void @_ZN27ConstructorWithNSArrayParamC1EP7NSArray(ptr [[VAR]], ptr %{{[0-9]+}})
  return ConstructorWithNSArrayParam([])
}
