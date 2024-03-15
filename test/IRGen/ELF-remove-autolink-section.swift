// RUN: %swiftc_driver -emit-ir %s -o - -Xfrontend -disable-implicit-concurrency-module-import -Xfrontend -disable-implicit-string-processing-module-import | %FileCheck %s -check-prefix ELF

// Check that the swift auto link section is available in the object file.
// RUN: %swiftc_driver -c %s -o %t -Xfrontend -disable-implicit-concurrency-module-import
// RUN: llvm-readelf %t -S | %FileCheck %s -check-prefix SECTION

// Checks that the swift auto link section is removed from the final binary.
// RUN: %swiftc_driver  -emit-executable %s -o %t -Xfrontend -disable-implicit-concurrency-module-import
// RUN: llvm-readelf %t -S | %FileCheck %s -check-prefix NOSECTION

// REQUIRES: OS=linux-gnu

print("Hi from Swift!")

// ELF: @llvm.used = {{.*}}ptr @_swift1_autolink_entries

// SECTION: .swift1_autolink_entries
// NOSECTION-NOT: .swift1_autolink_entries
