// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// Verify that symbolic interfaces are emitted.
//
// RUN: %target-swift-frontend %t/test.swift -I %t/Inputs -c -index-system-modules -index-store-path %t/store -enable-experimental-cxx-interop -Rindexing-system-module 2>%t/remarks
// RUN: echo "EOF" >> %t/remarks
// RUN: cat %t/remarks | %FileCheck --check-prefixes=REMARK_NEW,REMARK_INITIAL %s
// RUN: ls %t/store/interfaces | %FileCheck --check-prefix=FILES %s
// RUN: cat %t/store/interfaces/CxxModule* | %FileCheck --check-prefix=CHECK %s

// Verify that symbolic interfaces are not emitted when PCM doesn't change.
//
// RUN: %target-swift-frontend %t/test.swift -I %t/Inputs -c -index-system-modules -index-store-path %t/store -enable-experimental-cxx-interop -Rindexing-system-module 2>&1 | %FileCheck --check-prefix=REMARK_NO_UPDATE %s
// RUN: ls %t/store/interfaces | %FileCheck --check-prefix=FILES %s
// RUN: cat %t/store/interfaces/CxxModule* | %FileCheck --check-prefix=CHECK %s

// Verify that symbolic interface is re-emitted when the interface is removed.
//
// RUN: rm -r %t/store/interfaces
// RUN: %target-swift-frontend %t/test.swift -I %t/Inputs -c -index-system-modules -index-store-path %t/store -enable-experimental-cxx-interop -Rindexing-system-module 2>&1 | %FileCheck --check-prefix=REMARK_NEW %s
// RUN: ls %t/store/interfaces | %FileCheck --check-prefix=FILES %s
// RUN: cat %t/store/interfaces/CxxModule* | %FileCheck --check-prefixes=CHECK %s

// Verify that symbolic interface is re-emitted when PCM changes.
//
// RUN: echo "using AdditionalAlias = int;" >> %t/Inputs/headerA.h
// RUN: %target-swift-frontend %t/test.swift -I %t/Inputs -c -index-system-modules -index-store-path %t/store -enable-experimental-cxx-interop -Rindexing-system-module 2>&1 | %FileCheck --check-prefix=REMARK_NEW %s
// RUN: ls %t/store/interfaces | %FileCheck --check-prefix=FILES %s
// RUN: cat %t/store/interfaces/CxxModule* | %FileCheck --check-prefixes=CHECK,CHECK-UPDATED %s

//--- Inputs/module.modulemap
module CxxModule {
    header "headerA.h"
    header "headerB.h"
    requires cplusplus
}

module TransitiveCppMod {
    header "transitiveHeader.h"
    requires cplusplus
}

//--- Inputs/headerA.h

namespace ns {
    int freeFunction(int x, int y);
}

//--- Inputs/transitiveHeader.h

template<class T>
struct TransitiveStruct {
    T x;

    void test() {}
};

//--- Inputs/headerB.h

#include "headerA.h"
#include "transitiveHeader.h"

namespace ns {
    struct B {
        int y;
    };

    template<class T>
    struct TemplateRecord {
        void methodFunc(int x);

        struct InnerRecord {
            int innerMethod(int y);
        };

        template<class T2>
        struct InnerTemplate {
            void innerTemplateMethod();
        };

        InnerTemplate<int> returnsTemplateMethod();
    };

    inline namespace __1 {
        struct StructInInlineNamespace {
        };

        using TypealiasInInlineNamespace = TemplateRecord<StructInInlineNamespace>;
    }

    using TypealiasOfInlineNamespace = __1::StructInInlineNamespace;
}

using MyType = ns::TemplateRecord<int>;
using MyType2 = TransitiveStruct<int>;

//--- test.swift

import CxxModule

// REMARK_INITIAL: remark: emitting symbolic interface at {{.*}}{{/|\\}}interfaces{{/|\\}}CxxShim-{{.*}}.pcm.symbolicswiftinterface{{$}}
// REMARK_NEW: remark: emitting symbolic interface at {{.*}}{{/|\\}}interfaces{{/|\\}}CxxModule-{{.*}}.pcm.symbolicswiftinterface{{$}}
// REMARK_INITIAL-NEXT: EOF

// REMARK_NO_UPDATE: remark: emitting symbolic interface at {{.*}}{{/|\\}}interfaces{{/|\\}}CxxModule-{{.*}}.pcm.symbolicswiftinterface; skipping because it's up to date{{$}}

// FILES: CxxModule-{{.*}}.pcm.symbolicswiftinterface
// FILES-NOT: TransitiveCppMod

// CHECK: // Swift interface for module 'CxxModule'
// CHECK-UPDATED: typealias AdditionalAlias =
// CHECK:     enum ns {
// CHECK-EMPTY:
// CHECK-NEXT: struct B {
// CHECK-EMPTY:
// CHECK-NEXT:    init()
// CHECK-EMPTY:
// CHECK-NEXT:    init(y: Int32)
// CHECK-EMPTY:
// CHECK-NEXT:    var y: Int32
// CHECK-NEXT:  }
// CHECK-EMPTY:
// CHECK-NEXT:  struct TemplateRecord {
// CHECK-EMPTY:
// CHECK-NEXT:    @available(*, deprecated, message:
// CHECK-NEXT:    public init()
// CHECK-EMPTY:
// CHECK-NEXT:    mutating func methodFunc(_ x: Any)
// CHECK-EMPTY:
// CHECK-NEXT:    struct InnerRecord {
// CHECK-EMPTY:
// CHECK-NEXT:      @available(*, deprecated, message:
// CHECK-NEXT:      public init()
// CHECK-EMPTY:
// CHECK-NEXT:      mutating func innerMethod(_ y: Any)
// CHECK-NEXT:    }
// CHECK-EMPTY:
// CHECK-NEXT:    struct InnerTemplate {
// CHECK-EMPTY:
// CHECK-NEXT:    @available(*, deprecated, message:
// CHECK-NEXT:    public init()
// CHECK-EMPTY:
// CHECK-NEXT:      mutating func innerTemplateMethod()
// CHECK-NEXT:    }
// CHECK-EMPTY:
// CHECK-NEXT:    mutating func returnsTemplateMethod()
// CHECK-NEXT:  }
// CHECK:       public struct StructInInlineNamespace {
// CHECK-EMPTY:
// CHECK-NEXT:    public init()
// CHECK-NEXT:  }
// CHECK-EMPTY:
// CHECK-NEXT:  public typealias TypealiasInInlineNamespace = ns.TemplateRecord
// CHECK-EMPTY:
// CHECK-EMPTY:
// CHECK-NEXT:  public typealias TypealiasOfInlineNamespace = ns.StructInInlineNamespace
// CHECK-EMPTY:
// CHECK-NEXT:  static func freeFunction(_ x: Int32, _ y: Int32) -> Int32
// CHECK-NEXT: }
// CHECK-EMPTY:
// CHECK-NEXT: typealias MyType = ns.TemplateRecord
// CHECK-EMPTY:
// CHECK-NEXT: typealias MyType2 = TransitiveStruct
