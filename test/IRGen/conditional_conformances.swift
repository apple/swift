// RUN: %target-swift-frontend -disable-generic-metadata-prespecialization -emit-ir %S/../Inputs/conditional_conformance_basic_conformances.swift | %FileCheck %S/../Inputs/conditional_conformance_basic_conformances.swift --check-prefix=CHECK --check-prefix=CHECK-STABLE-ABI-%target-mandates-stable-abi
// RUN: %target-swift-frontend -disable-generic-metadata-prespecialization -emit-ir %S/../Inputs/conditional_conformance_with_assoc.swift | %FileCheck %S/../Inputs/conditional_conformance_with_assoc.swift --check-prefix=CHECK --check-prefix=CHECK-STABLE-ABI-%target-mandates-stable-abi
// RUN: %target-swift-frontend -disable-generic-metadata-prespecialization -emit-ir %S/../Inputs/conditional_conformance_subclass.swift | %FileCheck %S/../Inputs/conditional_conformance_subclass.swift --check-prefix=CHECK --check-prefix=CHECK-STABLE-ABI-%target-mandates-stable-abi
// RUN: %target-swift-frontend -disable-generic-metadata-prespecialization -emit-ir %S/../Inputs/conditional_conformance_recursive.swift | %FileCheck %S/../Inputs/conditional_conformance_recursive.swift --check-prefix=CHECK --check-prefix=CHECK-STABLE-ABI-%target-mandates-stable-abi

// Too many pointer-sized integers in the IR
// REQUIRES: PTRSIZE=64
