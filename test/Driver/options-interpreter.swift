// RUN: not %swift_driver_plain -deprecated-integrated-repl -emit-module 2>&1 | %FileCheck -check-prefix=IMMEDIATE_NO_MODULE %s
// RUN: not %swift_driver_plain -emit-module 2>&1 | %FileCheck -check-prefix=IMMEDIATE_NO_MODULE %s
// REQUIRES: swift_interpreter
// IMMEDIATE_NO_MODULE: error: option '-emit-module' is not supported by 'swift'; did you mean to use 'swiftc'?

// RUN: %swift_driver_plain -### %s | %FileCheck -check-prefix INTERPRET %s
// INTERPRET: -interpret

// RUN: %swift_driver_plain -### %s a b c | %FileCheck -check-prefix ARGS %s
// ARGS: -- a b c

// RUN: %swift_driver_plain -### -parse-stdlib %s | %FileCheck -check-prefix PARSE_STDLIB %s
// RUN: %swift_driver_plain -### -parse-stdlib | %FileCheck -check-prefix PARSE_STDLIB %s
// PARSE_STDLIB: -parse-stdlib


// RUN: %swift_driver_plain -sdk "" -### -target x86_64-apple-macosx10.9 -resource-dir /RSRC/ %s | %FileCheck -check-prefix=CHECK-RESOURCE-DIR-ONLY %s
// CHECK-RESOURCE-DIR-ONLY: # DYLD_LIBRARY_PATH=/RSRC/macosx{{$}}

// RUN: %swift_driver_plain -sdk "" -### -target x86_64-unknown-linux-gnu -resource-dir /RSRC/ %s | %FileCheck -check-prefix=CHECK-RESOURCE-DIR-ONLY-LINUX${LD_LIBRARY_PATH+_LAX} %s
// CHECK-RESOURCE-DIR-ONLY-LINUX: # LD_LIBRARY_PATH=/RSRC/linux{{$}}
// CHECK-RESOURCE-DIR-ONLY-LINUX_LAX: # LD_LIBRARY_PATH=/RSRC/linux{{$|:}}

// RUN: %swift_driver_plain -sdk "" -### -target x86_64-apple-macosx10.9 -L/foo/ %s | %FileCheck -check-prefix=CHECK-L %s
// CHECK-L: # DYLD_LIBRARY_PATH={{/foo/:[^:]+/lib/swift/macosx$}}

// RUN: %swift_driver_plain -sdk "" -### -target x86_64-apple-macosx10.9 -L/foo/ -L/bar/ %s | %FileCheck -check-prefix=CHECK-L2 %s
// CHECK-L2: # DYLD_LIBRARY_PATH={{/foo/:/bar/:[^:]+/lib/swift/macosx$}}

// RUN: env DYLD_LIBRARY_PATH=/abc/ SDKROOT=/sdkroot %swift_driver_plain -### -target x86_64-apple-macosx10.9 -L/foo/ -L/bar/ %s 2>&1 | %FileCheck -check-prefix=CHECK-L2-ENV %s 
// CHECK-L2-ENV: # DYLD_LIBRARY_PATH={{/foo/:/bar/:[^:]+/lib/swift/macosx:/sdkroot/usr/lib/swift:/abc/$}}

// RUN: %swift_driver_plain -### -target x86_64-apple-macosx10.9 %s | %FileCheck -check-prefix=CHECK-NO-FRAMEWORKS %s
// RUN: env DYLD_FRAMEWORK_PATH=/abc/ %swift_driver_plain -### -target x86_64-apple-macosx10.9 %s | %FileCheck -check-prefix=CHECK-NO-FRAMEWORKS %s
// CHECK-NO-FRAMEWORKS-NOT: DYLD_FRAMEWORK_PATH

// RUN: %swift_driver_plain -### -target x86_64-apple-macosx10.9 -F/foo/ %s | %FileCheck -check-prefix=CHECK-F %s
// CHECK-F: -F /foo/
// CHECK-F: #
// CHECK-F: DYLD_FRAMEWORK_PATH=/foo/{{$}}

// RUN: %swift_driver_plain -### -target x86_64-apple-macosx10.9 -F/foo/ -F/bar/ %s | %FileCheck -check-prefix=CHECK-F2 %s
// CHECK-F2: -F /foo/
// CHECK-F2: -F /bar/
// CHECK-F2: #
// CHECK-F2: DYLD_FRAMEWORK_PATH=/foo/:/bar/{{$}}

// RUN: env DYLD_FRAMEWORK_PATH=/abc/ %swift_driver_plain -### -target x86_64-apple-macosx10.9 -F/foo/ -F/bar/ %s | %FileCheck -check-prefix=CHECK-F2-ENV %s
// CHECK-F2-ENV: -F /foo/
// CHECK-F2-ENV: -F /bar/
// CHECK-F2-ENV: #
// CHECK-F2-ENV: DYLD_FRAMEWORK_PATH=/foo/:/bar/:/abc/{{$}}

// RUN: env DYLD_FRAMEWORK_PATH=/abc/ SDKROOT=/sdkroot %swift_driver_plain -### -target x86_64-apple-macosx10.9 -F/foo/ -F/bar/ -L/foo2/ -L/bar2/ %s 2>&1 | %FileCheck -check-prefix=CHECK-COMPLEX %s
// CHECK-COMPLEX: -F /foo/
// CHECK-COMPLEX: -F /bar/
// CHECK-COMPLEX: #
// CHECK-COMPLEX-DAG: DYLD_FRAMEWORK_PATH=/foo/:/bar/:/abc/{{$| }}
// CHECK-COMPLEX-DAG: DYLD_LIBRARY_PATH={{/foo2/:/bar2/:[^:]+/lib/swift/macosx:/sdkroot/usr/lib/swift($| )}}

// RUN: %swift_driver_plain -sdk /sdk -### -target aarch64-unknown-linux-gnu %s | %FileCheck -check-prefix=CHECK-RUNTIME-LIBRARY-PATH %s
// CHECK-RUNTIME-LIBRARY-PATH-NOT: # LD_LIBRARY_PATH=/sdk/usr/lib/swift/linux:/sdk/usr/lib/swift

// RUN: %swift_driver_plain -sdk "" -### -target x86_64-unknown-linux-gnu -L/foo/ %s | %FileCheck -check-prefix=CHECK-L-LINUX${LD_LIBRARY_PATH+_LAX} %s
// CHECK-L-LINUX: # LD_LIBRARY_PATH={{/foo/:[^:]+/lib/swift/linux$}}
// CHECK-L-LINUX_LAX: # LD_LIBRARY_PATH={{/foo/:[^:]+/lib/swift/linux($|:)}}

// RUN: env LD_LIBRARY_PATH=/abc/ %swift_driver_plain -### -target x86_64-unknown-linux-gnu -L/foo/ -L/bar/ %s | %FileCheck -check-prefix=CHECK-LINUX-COMPLEX${LD_LIBRARY_PATH+_LAX} %s
// CHECK-LINUX-COMPLEX: # LD_LIBRARY_PATH={{/foo/:/bar/:[^:]+/lib/swift/linux:/abc/$}}
// CHECK-LINUX-COMPLEX_LAX: # LD_LIBRARY_PATH={{/foo/:/bar/:[^:]+/lib/swift/linux:/abc/($|:)}}
