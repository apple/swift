// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: sed -e "s@DIR@%{/t:regex_replacement}@g" %t/MixedTarget/Sources/needsfixing-module.modulemap > %t/MixedTarget/Sources/module.modulemap

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -I %t/MixedTarget/Sources/ -module-name Mixed -import-underlying-module -parse-as-library %t/MixedTarget/Sources/test.swift -typecheck -emit-clang-header-path %t/mixed.h

// RUN: %FileCheck -check-prefix=CHECK-HDR %s <  %t/mixed.h

// RUN: sed -e "s@DIR@%{/t:regex_replacement}@g" %t/MixedTarget/Sources/needsfixing-umbrella-module.modulemap > %t/MixedTarget/Sources/module.modulemap

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -I %t/MixedTarget/Sources/ -module-name Mixed -import-underlying-module -parse-as-library %t/MixedTarget/Sources/test.swift -typecheck -emit-clang-header-path %t/mixed-umbrella.h

// RUN: %FileCheck -check-prefix=CHECK-UMB %s <  %t/mixed-umbrella.h

// REQUIRES: objc_interop

//--- MixedTarget/Sources/needsfixing-module.modulemap

module Mixed {
  umbrella "DIR/MixedTarget/Sources"
  export *
}

//--- MixedTarget/Sources/needsfixing-umbrella-module.modulemap

module Mixed {
  umbrella header "DIR/MixedTarget/Sources/include/MixedTarget.h"
  export *
}

//--- MixedTarget/Sources/include/MixedTarget.h
#pragma once

#import "test.h"

//--- MixedTarget/Sources/include/test.h
#pragma once

void cFunc();

typedef int MyType;

//--- MixedTarget/Sources/empty.h
#pragma once

// empty header

//--- MixedTarget/Sources/test.swift

@_cdecl("test")
public func test() -> MyType {
  cFunc()
  return 0
}

// CHECK-HDR: #if __has_feature(objc_modules)
// CHECK-HDR-NEXT: #if __has_warning("-Watimport-in-framework-header")
// CHECK-HDR-NEXT: #pragma clang diagnostic ignored "-Watimport-in-framework-header"
// CHECK-HDR-NEXT: #endif
// CHECK-HDR-NEXT: #endif
// CHECK-HDR-EMPTY:
// CHECK-HDR-NEXT: #import <{{.*}}MixedTarget{{[/\\]}}Sources{{[/\\]}}include{{[/\\]}}test.h>
// CHECK-HDR-NEXT: #import <{{.*}}MixedTarget{{[/\\]}}Sources{{[/\\]}}include{{[/\\]}}MixedTarget.h>
// CHECK-HDR-NEXT: #import <{{.*}}MixedTarget{{[/\\]}}Sources{{[/\\]}}empty.h>
// CHECK-HDR-NEXT: #endif

// CHECK-UMB: #if __has_feature(objc_modules)
// CHECK-UMB-NEXT: #if __has_warning("-Watimport-in-framework-header")
// CHECK-UMB-NEXT: #pragma clang diagnostic ignored "-Watimport-in-framework-header"
// CHECK-UMB-NEXT: #endif
// CHECK-UMB-NEXT: #endif
// CHECK-UMB-EMPTY:
// CHECK-UMB-NEXT: #import <{{.*}}MixedTarget{{[/\\]}}Sources{{[/\\]}}include{{[/\\]}}MixedTarget.h>
// CHECK-UMB-NEXT: #endif
