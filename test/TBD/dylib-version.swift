// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-ir -o /dev/null %s -tbd-current-version 2.0.3 -tbd-compatibility-version 1.7 -emit-tbd -emit-tbd-path %t/both_provided.tbd
// RUN: %target-swift-frontend -emit-ir -o /dev/null %s -tbd-current-version 2.0 -emit-tbd -emit-tbd-path %t/only_current_provided.tbd
// RUN: %target-swift-frontend -emit-ir -o /dev/null %s -tbd-compatibility-version 2 -emit-tbd -emit-tbd-path %t/only_compat_provided.tbd

// RUN: %FileCheck %s --check-prefix BOTH < %t/both_provided.tbd
// RUN: %FileCheck %s --check-prefix CURRENT < %t/only_current_provided.tbd
// RUN: %FileCheck %s --check-prefix COMPAT < %t/only_compat_provided.tbd

// BOTH: current-version: 2.0.3
// BOTH: compatibility-version: 1.7
// CURRENT: current-version: 2

// Compatibility version defaults to 1 if not present in TBD file, and
// tapi does not write field if compatibility version is 1

// CURRENT-NOT: compatibility-version: 1

// COMPAT: compatibility-version: 2

// Same as above -- current version defaults to 1 and is not present in
// emitted TBD file if it's 1.
// COMPAT-NOT: current-version: 1
