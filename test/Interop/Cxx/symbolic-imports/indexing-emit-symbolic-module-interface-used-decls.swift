// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend %t/test.swift -I %t/Inputs -c -index-system-modules -index-store-path %t/store -enable-experimental-cxx-interop 2>&1
// RUN: cat %t/store/interfaces/CxxModule* | %FileCheck --check-prefix=CHECK %s

//--- Inputs/module.modulemap
module CxxModule {
    header "header.h"
    requires cplusplus
}

//--- Inputs/header.h

namespace ns {
    template<class T>
    struct TemplateRecord {
        void methodFunc(int x) const;
    };
}

using TemplateRecordInt = ns::TemplateRecord<int>;

//--- test.swift

import CxxModule

public func useConcreteTemplate() {
    let x = TemplateRecordInt()
    x.methodFunc(2)
}

// CHECK: // Swift interface for module 'CxxModule'
// CHECK:     public enum ns {
// CHECK-EMPTY:
// CHECK-NEXT: public struct TemplateRecord {
// CHECK-EMPTY:
// CHECK-NEXT:    @available(*, deprecated, message:
// CHECK-NEXT:    public init()
// CHECK-EMPTY:
// CHECK-NEXT:    public func methodFunc(_ x: Any)
// CHECK-NEXT:}
// CHECK-NEXT:}
// CHECK-EMPTY:
// CHECK-NEXT: public typealias TemplateRecordInt = ns.TemplateRecord
