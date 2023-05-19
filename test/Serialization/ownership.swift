// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -emit-module -o %t %s
// RUN: %target-swift-ide-test(mock-sdk: %clang-importer-sdk) -print-module -skip-deinit=false -module-to-print=ownership -I %t -source-filename=%s | %FileCheck %s

// REQUIRES: objc_interop

import Foundation

// CHECK-LABEL: class PropertyOwnership {
class PropertyOwnership {
  // CHECK-NEXT:  @NSCopying var copying: (any NSCopying)?
  @NSCopying var copying: NSCopying?
  // CHECK-NEXT:  weak var weakVar: @sil_weak AnyObject?
  weak var weakVar: AnyObject?
  // CHECK-NEXT:  unowned var unownedVar: @sil_unowned PropertyOwnership
  unowned var unownedVar: PropertyOwnership
  // CHECK-NEXT:  unowned(unsafe) var unownedUnsafeVar: @sil_unmanaged PropertyOwnership
  unowned(unsafe) var unownedUnsafeVar: PropertyOwnership

  // CHECK-NEXT:  init(other: PropertyOwnership)
  init(other: PropertyOwnership) {
    unownedVar = other
    unownedUnsafeVar = other
  }
  // CHECK-NEXT:  deinit
} // CHECK-NEXT: }
