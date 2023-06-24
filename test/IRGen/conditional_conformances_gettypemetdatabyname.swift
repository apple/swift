// RUN: %target-swift-frontend -disable-generic-metadata-prespecialization -target %target-cpu-apple-macosx10.15.4 -emit-ir %S/../Inputs/conditional_conformance_basic_conformances.swift | %FileCheck %S/../Inputs/conditional_conformance_basic_conformances.swift --check-prefix=TYPEBYNAME
// RUN: %target-swift-frontend -prespecialize-generic-metadata -target %target-cpu-apple-macosx99.99 -emit-ir %S/../Inputs/conditional_conformance_basic_conformances.swift | %FileCheck %S/../Inputs/conditional_conformance_basic_conformances.swift --check-prefix=TYPEBYNAME_PRESPECIALIZED
// RUN: %target-swift-frontend -target %target-cpu-apple-macosx10.15.4 -emit-ir %S/../Inputs/conditional_conformance_basic_conformances.swift | %FileCheck %S/../Inputs/conditional_conformance_basic_conformances.swift --check-prefix=CHECK --check-prefix=CHECK-STABLE-ABI-TRUE

// Too many pointer-sized integers in the IR
// REQUIRES: PTRSIZE=64
// REQUIRES: OS=macosx
