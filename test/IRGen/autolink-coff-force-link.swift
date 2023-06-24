// RUN: %empty-directory(%t)

// RUN: %swift -target i686--windows-msvc -parse-stdlib -autolink-force-load -module-name swiftMSVCRT -module-link-name swiftMSVCRT -emit-module -o %t/swiftMSVCRT.swiftmodule %S/../Inputs/empty.swift

// RUN: %swift %use_no_opaque_pointers -target i686--windows-msvc -parse-as-library -parse-stdlib -autolink-force-load -module-name autolink -module-link-name autolink -emit-ir -o - %s -I%t | %FileCheck %s
// RUN: %swift %use_no_opaque_pointers -target i686--windows-msvc -parse-as-library -parse-stdlib -autolink-force-load -module-name autolink -module-link-name autolink -S -o - %s -I%t | %FileCheck %s -check-prefix CHECK-ASM-MSC
// RUN: %swift -target i686--windows-msvc -parse-as-library -parse-stdlib -autolink-force-load -module-name autolink -module-link-name autolink -emit-ir -o - %s -I%t
// RUN: %swift -target i686--windows-msvc -parse-as-library -parse-stdlib -autolink-force-load -module-name autolink -module-link-name autolink -S -o - %s -I%t

// RUN: %swift -target i686--windows-itanium -parse-stdlib -autolink-force-load -module-name swiftMSVCRT -module-link-name swiftMSVCRT -emit-module -o %t/swiftMSVCRT.swiftmodule %S/../Inputs/empty.swift

// RUN: %swift %use_no_opaque_pointers -target i686--windows-itanium -parse-as-library -parse-stdlib -autolink-force-load -module-name autolink -module-link-name autolink -emit-ir -o - %s -I%t | %FileCheck %s
// RUN: %swift %use_no_opaque_pointers -target i686--windows-itanium -parse-as-library -parse-stdlib -autolink-force-load -module-name autolink -module-link-name autolink -S -o - %s -I%t | %FileCheck %s -check-prefix CHECK-ASM-GNU
// RUN: %swift -target i686--windows-itanium -parse-as-library -parse-stdlib -autolink-force-load -module-name autolink -module-link-name autolink -emit-ir -o - %s -I%t
// RUN: %swift -target i686--windows-itanium -parse-as-library -parse-stdlib -autolink-force-load -module-name autolink -module-link-name autolink -S -o - %s -I%t

// REQUIRES: OS=windows-msvc

import swiftMSVCRT

// CHECK: @"_swift_FORCE_LOAD_$_swiftMSVCRT_$_autolink" = weak_odr hidden constant void ()* @"_swift_FORCE_LOAD_$_swiftMSVCRT"
// CHECK: define dllexport void @"_swift_FORCE_LOAD_$_autolink"()

// CHECK-ASM-GNU: .ascii " -export:\"__swift_FORCE_LOAD_$_autolink\""
// CHECK-ASM-MSC: .ascii " /EXPORT:\"__swift_FORCE_LOAD_$_autolink\""
