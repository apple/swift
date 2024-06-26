// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: sed -e "s@DIR@%{/t:regex_replacement}@g" %t/MixedTarget/Sources/needsfixing-module.modulemap > %t/MixedTarget/Sources/module.modulemap

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -I %t/MixedTarget/Sources/ -module-name Mixed -import-underlying-module -parse-as-library %t/MixedTarget/Sources/test.swift -typecheck -emit-clang-header-path %t/mixed.h -cxx-interoperability-mode=default

// RUN: %FileCheck -check-prefixes=CHECK,CHECK-HDR %s <  %t/mixed.h

// RUN: %check-interop-cxx-file-in-clang(-I %t %t/test.cpp -Wno-import-preprocessor-directive-pedantic)

// RUN: sed -e "s@DIR@%{/t:regex_replacement}@g" %t/MixedTarget/Sources/needsfixing-umbrella-module.modulemap > %t/MixedTarget/Sources/module.modulemap

// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -I %t/MixedTarget/Sources/ -module-name Mixed -import-underlying-module -parse-as-library %t/MixedTarget/Sources/test.swift -typecheck -emit-clang-header-path %t/mixed-umbrella.h -cxx-interoperability-mode=default

// RUN: %FileCheck -check-prefixes=CHECK,CHECK-UMB %s <  %t/mixed-umbrella.h

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

typedef int MyType;

//--- MixedTarget/Sources/include/test.h
#pragma once

class CxxClass {
public:
    void test() const;

    int x;
};

//--- MixedTarget/Sources/empty.h
#pragma once

// empty header

//--- MixedTarget/Sources/test.swift

public func makeCxxClass() -> CxxClass {
  let c = CxxClass(x: 2)
  c.test()
  return c
}

//--- test.cpp

#include "mixed.h"

void testFunc();
void testFunc() {
  CxxClass v = Mixed::makeCxxClass();
  (void)v;
}

// CHECK: #if __has_feature(objc_modules)
// CHECK-NEXT: #if __has_warning("-Watimport-in-framework-header")
// CHECK-NEXT: #pragma clang diagnostic ignored "-Watimport-in-framework-header"
// CHECK-NEXT: #endif
// CHECK-NEXT: #endif
// CHECK-EMPTY:
// CHECK-NEXT: #endif

// CHECK-HDR: #import <{{.*}}MixedTarget{{[/\\]}}Sources{{[/\\]}}include{{[/\\]}}test.h>
// CHECK-HDR: #import <{{.*}}MixedTarget{{[/\\]}}Sources{{[/\\]}}include{{[/\\]}}MixedTarget.h>
// CHECK-HDR: #import <{{.*}}MixedTarget{{[/\\]}}Sources{{[/\\]}}empty.h>

// CHECK-UMB: #import <{{.*}}MixedTarget{{[/\\]}}Sources{{[/\\]}}include{{[/\\]}}MixedTarget.h>
