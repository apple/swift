// Verify that when the legacy driver (swift-frontend executable invoked as
// swiftc) spawns the new driver, it passes the original arguments (i.e.,
// preserving response files) instead of trying to spawn the process with the
// expansion, which may exceed `ARG_MAX`.

// REQUIRES: shell
// RUN: %{python} -c 'for i in range(500001): print("-DTEST5_" + str(i))' > %t.resp
// RUN: cp %S/Inputs/print-args.sh %swift-bin-dir/legacy-driver-propagates-response-file.sh
// RUN: env SWIFT_USE_NEW_DRIVER=legacy-driver-propagates-response-file.sh %swiftc_driver_plain %s @%t.resp | %FileCheck %s
// RUN: rm %swift-bin-dir/legacy-driver-propagates-response-file.sh

// CHECK:      -Xfrontend
// CHECK-NEXT: -new-driver-path
// CHECK-NEXT: -Xfrontend
// CHECK-NEXT: legacy-driver-propagates-response-file.sh
// CHECK:      @{{.*}}.resp
// CHECK-NOT:  -DTEST5_{{.*}}
