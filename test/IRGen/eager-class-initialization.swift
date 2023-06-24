// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) %use_no_opaque_pointers  %s -emit-ir -target %target-pre-stable-abi-triple | %FileCheck %s -DINT=i%target-ptrsize --check-prefix=CHECK --check-prefix=CHECK-OLD
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) %s -emit-ir -target %target-pre-stable-abi-triple

// REQUIRES: objc_interop
// UNSUPPORTED: swift_only_stable_abi

// See also eager-class-initialization-stable-abi.swift, for the stable ABI
// deployment target test.

import Foundation

class BaseWithCoding : NSObject, NSCoding {
  required init(coder: NSCoder) { fatalError() }

  func encode(coder: NSCoder) { fatalError() }
}

class Generic<T> : BaseWithCoding {}

class GenericAncestry : Generic<Int> {}

@objc(custom_name)
class GenericAncestryWithCustomName : Generic<Double> {}

// CHECK-LABEL: define {{.*}} @_swift_eager_class_initialization
// CHECK-NEXT:  entry:
// CHECK-OLD-NEXT: [[RESPONSE:%.*]] = call swiftcc %swift.metadata_response @"$s4main15GenericAncestryCMa"([[INT]] 0)
// CHECK-OLD-NEXT: [[METADATA:%.*]] = extractvalue %swift.metadata_response [[RESPONSE]], 0
// CHECK-OLD-NEXT: call void asm sideeffect "", "r"(%swift.type* [[METADATA]])
// CHECK-NEXT:     [[RESPONSE:%.*]] = call swiftcc %swift.metadata_response @"$s4main29GenericAncestryWithCustomNameCMa"([[INT]] 0)
// CHECK-NEXT:     [[METADATA:%.*]] = extractvalue %swift.metadata_response [[RESPONSE]], 0
// CHECK-NEXT:     call void asm sideeffect "", "r"(%swift.type* [[METADATA]])
// CHECK-NEXT:     ret
