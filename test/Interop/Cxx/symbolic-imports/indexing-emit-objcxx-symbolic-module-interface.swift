// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend %t/test.swift -I %t/Inputs -c -index-system-modules -index-store-path %t/store -enable-experimental-cxx-interop -Rindexing-system-module 2>&1 | %FileCheck --check-prefix=REMARK_NEW %s
// RUN: ls %t/store/interfaces | %FileCheck --check-prefix=FILES %s
// RUN: cat %t/store/interfaces/ObjCxxModule* | %FileCheck --check-prefix=CHECK %s


// Verify that symbolic interface is not emitted without interop.
//
// RUN: rm -r %t/store/interfaces
// RUN: %target-swift-frontend %t/test.swift -I %t/Inputs -c -index-system-modules -index-store-path %t/store -Rindexing-system-module 2>&1 > %t/out
// RUN: echo "non-empty-file-check" >> %t/out
// RUN: cat %t/out | %FileCheck --check-prefix=REMARK_NONE %s
// RUN: not ls %t/store/interfaces

// REQUIRES: objc_interop
// REQUIRES: cxx-interop-fixed-cf_options

//--- Inputs/module.modulemap
module ObjCxxModule {
    header "headerA.h"
    // NOTE: does not require cplusplus
}

//--- Inputs/headerA.h

#include <Foundation/Foundation.h>

#ifdef __cplusplus
namespace ns {
    int freeCxxFunction(int x, int y);
}
#endif

@interface ObjCClass: NSObject

- (void)myTestMethod;

@end

//--- test.swift

import ObjCxxModule

// REMARK_NEW: remark: emitting symbolic interface at {{.*}}/interfaces/ObjCxxModule-{{.*}}.pcm.symbolicswiftinterface{{$}}
// REMARK_NONE-NOT: emitting symbolic interface at

// FILES: ObjCxxModule-{{.*}}.pcm.symbolicswiftinterface

// CHECK: // Swift interface for module 'ObjCxxModule'
// CHECK-NEXT: import Foundation
// CHECK-EMPTY:
// CHECK-NEXT: public enum ns {
// CHECK-EMPTY:
// CHECK-NEXT:     public static func freeCxxFunction(_ x: Int32, _ y: Int32) -> Int32
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: open class ObjCClass : NSObject {
// CHECK-EMPTY:
// CHECK-NEXT:     open func myTestMethod()
// CHECK-NEXT: }
