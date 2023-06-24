// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -DA -parse-as-library -parse-stdlib -module-name A %s -o %t/A.swiftmodule
// RUN: %target-swift-frontend -emit-ir -DB -I %t -parse-as-library -parse-stdlib -module-name B -o - %s | %FileCheck %s -check-prefix CHECK-%target-import-type

#if A
public protocol P {
}

public enum E : P {
}
#endif

#if B
import A

public protocol Q : P {
}

extension E : Q {
}
#endif

// CHECK-DIRECT: @"$s1A1EO1B1QADWP" ={{( dllexport)?}}{{( protected)?}} constant [2 x ptr] [ptr @"$s1A1EO1B1QADMc", ptr @"$s1A1EOAA1PAAWP"]
// CHECK-INDIRECT: @"$s1A1EO1B1QADWP" ={{( dllexport)?}}{{( protected)?}} constant [2 x ptr] [ptr @"$s1A1EO1B1QADMc", ptr null]

