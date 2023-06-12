// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -target x86_64-apple-macosx10.9 %s | %FileCheck -check-prefix=CHECK -check-prefix=OSX %s

// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -target x86_64-apple-ios7.1-simulator -resource-dir %S/Inputs/fake-resource-dir-old/lib/swift/ %s | %FileCheck -check-prefix=CHECK -check-prefix=IOS %s
// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -target arm64-apple-ios7.1 -resource-dir %S/Inputs/fake-resource-dir-old/lib/swift/ %s | %FileCheck -check-prefix=CHECK -check-prefix=IOS %s

// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -target x86_64-apple-ios7.1-simulator -resource-dir %S/Inputs/fake-resource-dir/lib/swift/ %s | %FileCheck -check-prefix=CHECK -check-prefix=IOSSIM %s
// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -target arm64-apple-ios7.1 -resource-dir %S/Inputs/fake-resource-dir/lib/swift/ %s | %FileCheck -check-prefix=CHECK -check-prefix=IOS %s

// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -target x86_64-apple-tvos9.0-simulator -resource-dir %S/Inputs/fake-resource-dir-old/lib/swift/ %s | %FileCheck -check-prefix=CHECK -check-prefix=tvOS %s
// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -target arm64-apple-tvos9.0 -resource-dir %S/Inputs/fake-resource-dir-old/lib/swift/ %s | %FileCheck -check-prefix=CHECK -check-prefix=tvOS %s

// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -target x86_64-apple-tvos9.0-simulator -resource-dir %S/Inputs/fake-resource-dir/lib/swift/ %s | %FileCheck -check-prefix=CHECK -check-prefix=tvOS_SIM %s
// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -target arm64-apple-tvos9.0 -resource-dir %S/Inputs/fake-resource-dir/lib/swift/ %s | %FileCheck -check-prefix=CHECK -check-prefix=tvOS %s

// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -target i386-apple-watchos2.0-simulator -resource-dir %S/Inputs/fake-resource-dir-old/lib/swift/ %s | %FileCheck -check-prefix=CHECK -check-prefix=watchOS %s
// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -target armv7k-apple-watchos2.0 -resource-dir %S/Inputs/fake-resource-dir-old/lib/swift/ %s | %FileCheck -check-prefix=CHECK -check-prefix=watchOS %s
// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -target i386-apple-watchos2.0-simulator -resource-dir %S/Inputs/fake-resource-dir/lib/swift/ %s | %FileCheck -check-prefix=CHECK -check-prefix=watchOS_SIM %s
// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -target armv7k-apple-watchos2.0 -resource-dir %S/Inputs/fake-resource-dir/lib/swift/ %s | %FileCheck -check-prefix=CHECK -check-prefix=watchOS %s

// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -target x86_64-unknown-linux-gnu %s | %FileCheck -check-prefix=CHECK -check-prefix=LINUX %s
// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -target x86_64-unknown-windows-msvc %s | %FileCheck -check-prefix=CHECK -check-prefix=WINDOWS %s

// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -target wasm32-unknown-wasi -resource-dir %S/Inputs/fake-resource-dir/lib/swift_static %s | %FileCheck -check-prefix CHECK -check-prefix WASI %s

// CHECK: swift
// CHECK: -profile-generate

// OSX: {{(bin/)?ld(.exe)?"? }}
// OSX: lib{{(\\\\|/)}}swift{{(\\\\|/)}}clang{{(\\\\|/)}}lib{{(\\\\|/)}}darwin{{(\\\\|/)}}libclang_rt.profile_osx.a

// IOS: {{(bin/)?ld(.exe)?"? }}
// IOS: lib{{(\\\\|/)}}swift{{(\\\\|/)}}clang{{(\\\\|/)}}lib{{(\\\\|/)}}darwin{{(\\\\|/)}}libclang_rt.profile_ios.a

// IOSSIM: {{(bin/)?ld(.exe)?"? }}
// IOSSIM: lib{{(\\\\|/)}}swift{{(\\\\|/)}}clang{{(\\\\|/)}}lib{{(\\\\|/)}}darwin{{(\\\\|/)}}libclang_rt.profile_iossim.a

// tvOS: {{(bin/)?ld(.exe)?"? }}
// tvOS: lib{{(\\\\|/)}}swift{{(\\\\|/)}}clang{{(\\\\|/)}}lib{{(\\\\|/)}}darwin{{(\\\\|/)}}libclang_rt.profile_tvos.a

// tvOS_SIM: {{(bin/)?ld(.exe)?"? }}
// tvOS_SIM: lib{{(\\\\|/)}}swift{{(\\\\|/)}}clang{{(\\\\|/)}}lib{{(\\\\|/)}}darwin{{(\\\\|/)}}libclang_rt.profile_tvossim.a

// watchOS: {{(bin/)?ld(.exe)?"? }}
// watchOS: lib{{(\\\\|/)}}swift{{(\\\\|/)}}clang{{(\\\\|/)}}lib{{(\\\\|/)}}darwin{{(\\\\|/)}}libclang_rt.profile_watchos.a

// watchOS_SIM: {{(bin/)?ld(.exe)?"? }}
// watchOS_SIM: lib{{(\\\\|/)}}swift{{(\\\\|/)}}clang{{(\\\\|/)}}lib{{(\\\\|/)}}darwin{{(\\\\|/)}}libclang_rt.profile_watchossim.a

// LINUX: clang{{(\.exe)?"? }}
// LINUX: lib{{(\\\\|/)}}swift{{(\\\\|/)}}clang{{(\\\\|/)}}lib{{(\\\\|/)}}linux{{(\\\\|/)}}libclang_rt.profile-x86_64.a
// LINUX: -u__llvm_profile_runtime

// WINDOWS: clang{{(\.exe)?"? }}
// WINDOWS: -Xlinker -include:__llvm_profile_runtime
// WINDOWS: -lclang_rt.profile

// WASI: clang{{(\.exe)?"? }}
// WASI: lib{{(\\\\|/)}}{{swift|swift_static}}{{(\\\\|/)}}clang{{(\\\\|/)}}lib{{(\\\\|/)}}wasi{{(\\\\|/)}}libclang_rt.profile-wasm32.a
// WASI: -u__llvm_profile_runtime

// RUN: not %swiftc_driver -sdk "" -driver-print-jobs -profile-generate -profile-use=/dev/null %s 2>&1 | %FileCheck -check-prefix=MIX_GEN_USE %s
// MIX_GEN_USE: conflicting options '-profile-generate' and '-profile-use'

// RUN: not %swiftc_driver -sdk "" -driver-print-jobs -profile-use=%t.does_not_exist %s 2>&1 | %FileCheck -check-prefix=USE_MISSING_FILE %s
// USE_MISSING_FILE: no profdata file exists at '{{[^']+}}.does_not_exist'

// RUN: %swiftc_driver -sdk "" -driver-print-jobs -profile-use=/dev/null %s | %FileCheck -check-prefix=USE_DEVNULL %s
// USE_DEVNULL: swift
// USE_DEVNULL: -profile-use={{/dev/null|(.*profiling-[^ ]*.o)}}
