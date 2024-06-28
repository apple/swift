// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// Verify that symbolic interfaces are emitted.
//
// RUN: %target-swift-frontend %t/test.swift -I %t/Inputs -c -index-system-modules -index-store-path %t/store -enable-experimental-cxx-interop -Rindexing-system-module 2>%t/remarks
// RUN: echo "EOF" >> %t/remarks
// RUN: cat %t/remarks | %FileCheck --check-prefix=REMARK_NEW %s
// RUN: ls %t/store/interfaces | %FileCheck --check-prefix=FILES %s
// RUN: cat %t/store/interfaces/std* | %FileCheck --check-prefix=CHECK %s

// Verify that symbolic interfaces are not emitted when PCM doesn't change.
//
// RUN: %target-swift-frontend %t/test.swift -I %t/Inputs -c -index-system-modules -index-store-path %t/store -enable-experimental-cxx-interop -Rindexing-system-module 2>&1 | %FileCheck --check-prefix=REMARK_NO_UPDATE %s
// RUN: ls %t/store/interfaces | %FileCheck --check-prefix=FILES %s

// REQUIRES: OS=macosx

//--- Inputs/module.modulemap
module CxxModule {
    header "headerA.h"
    requires cplusplus
}

//--- Inputs/headerA.h

int freeFunction(int x, int y);

//--- test.swift

import CxxStdlib
import CxxModule

// REMARK_NEW: remark: emitting symbolic interface at {{.*}}/interfaces/std-{{.*}}.pcm.symbolicswiftinterface{{$}}
// REMARK_NEW: remark: emitting symbolic interface at {{.*}}/interfaces/CxxModule-{{.*}}.pcm.symbolicswiftinterface{{$}}

// REMARK_NO_UPDATE-NOT: remark: emitting symbolic interface at {{.*}}/interfaces/std-{{.*}}.pcm.symbolicswiftinterface{{$}}
// REMARK_NO_UPDATE-NOT: remark: emitting symbolic interface at {{.*}}/interfaces/CxxModule-{{.*}}.pcm.symbolicswiftinterface{{$}}

// FILES: CxxModule-{{.*}}.pcm.symbolicswiftinterface
// FILES: std-{{.*}}.pcm.symbolicswiftinterface

// CHECK: // Swift interface for system module 'std'
