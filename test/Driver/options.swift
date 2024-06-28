// REQUIRES: swift_interpreter

// RUN: not %swiftc_driver -emit-silgen -parse-as-library %s -module-name "Swift" 2>&1 | %FileCheck -check-prefix=STDLIB_MODULE %s
// RUN: %target-swiftc_driver -emit-silgen -parse-as-library %s -module-name "Swift" -parse-stdlib -###
// STDLIB_MODULE: error: module name "Swift" is reserved for the standard library{{$}}

// RUN: not %swiftc_driver -crazy-option-that-does-not-exist %s 2>&1 | %FileCheck -check-prefix=INVALID_OPTION %s
// RUN: not %swift_driver -crazy-option-that-does-not-exist 2>&1 | %FileCheck -check-prefix=INVALID_OPTION %s
// INVALID_OPTION: error: unknown argument: '-crazy-option-that-does-not-exist'

// RUN: %swiftc_driver -assert-config Debug -### %s | %FileCheck -check-prefix=ASSERTCONFIG %s
// RUN: %swift_driver -assert-config Debug -### | %FileCheck -check-prefix=ASSERTCONFIG %s
// ASSERTCONFIG: -assert-config Debug

// RUN: %swiftc_driver -assert-config Release -### %s | %FileCheck -check-prefix=ASSERTCONFIG_RELEASE %s
// RUN: %swift_driver -assert-config Release -### %s | %FileCheck -check-prefix=ASSERTCONFIG_RELEASE %s
// ASSERTCONFIG_RELEASE: -assert-config Release

// RUN: %swiftc_driver -assert-config Unchecked -### %s | %FileCheck -check-prefix=ASSERTCONFIG_UNCHECKED %s
// RUN: %swift_driver -assert-config Unchecked -### %s | %FileCheck -check-prefix=ASSERTCONFIG_UNCHECKED %s
// ASSERTCONFIG_UNCHECKED: -assert-config Unchecked

// RUN: %swiftc_driver -assert-config DisableReplacement -### %s | %FileCheck -check-prefix=ASSERTCONFIG_DISABLEREPLACEMENT %s
// RUN: %swift_driver -assert-config DisableReplacement -### %s | %FileCheck -check-prefix=ASSERTCONFIG_DISABLEREPLACEMENT %s
// ASSERTCONFIG_DISABLEREPLACEMENT: -assert-config DisableReplacement

// RUN: not %swiftc_driver -import-objc-header fake.h -import-underlying-module -c %s 2>&1 | %FileCheck -check-prefix=FRAMEWORK_BRIDGING_HEADER %s
// FRAMEWORK_BRIDGING_HEADER: error: using bridging headers with framework targets is unsupported

// RUN: not %swiftc_driver -import-objc-header fake.h -emit-module-interface %s 2>&1 | %FileCheck -check-prefix=BRIDGING_HEADER_SWIFTINTERFACE %s
// RUN: not %swiftc_driver -import-objc-header fake.h -emit-module-interface-path fake.swiftinterface %s 2>&1 | %FileCheck -check-prefix=BRIDGING_HEADER_SWIFTINTERFACE %s
// BRIDGING_HEADER_SWIFTINTERFACE: error: using bridging headers with module interfaces is unsupported

// RUN: %swift_driver -### | %FileCheck -check-prefix=DEFAULT_REPL %s
// DEFAULT_REPL: -repl
// RUN: not %swiftc_driver 2>&1 | %FileCheck -check-prefix=DEFAULT_EXEC_ERR  %s
// DEFAULT_EXEC_ERR: error: no input files
// RUN: %swiftc_driver %s -### 2>&1 | %FileCheck -check-prefix=DEFAULT_EXEC  %s
// DEFAULT_EXEC: -c
// DEFAULT_EXEC: {{ld|clang\+\+}}

// RUN: %swift_driver -repl -### 2>&1 | %FileCheck -check-prefix=REPL %s
// REPL: warning: unnecessary option '-repl'

// RUN: %swift_driver -lldb-repl -### 2>&1 | %FileCheck -check-prefix=LLDB_REPL %s
// LLDB_REPL-NOT: warning
// LLDB_REPL: lldb
// LLDB_REPL-NOT: warning

// RUN: %swift_driver -deprecated-integrated-repl -### 2>&1 | %FileCheck -check-prefix=INTEGRATED_REPL %s
// INTEGRATED_REPL-NOT: warning
// INTEGRATED_REPL: -repl
// INTEGRATED_REPL-NOT: warning

// RUN: %swift_driver -### %s | %FileCheck -check-prefix=DEFAULT_I %s
// DEFAULT_I: -interpret

// RUN: not %swift_driver -### -i %s 2>&1 | %FileCheck -check-prefix=I_MODE %s
// RUN: not %swift_driver -### -i 2>&1 | %FileCheck -check-prefix=I_MODE %s
// RUN: not %swiftc_driver -### -i %s 2>&1 | %FileCheck -check-prefix=I_MODE %s
// RUN: not %swiftc_driver -### -i 2>&1 | %FileCheck -check-prefix=I_MODE %s
// I_MODE: error: the flag '-i' is no longer required and has been removed; use 'swift input-filename'

// RUN: not %swift_driver -### -c %s 2>&1 | %FileCheck -check-prefix=C_MODE %s
// C_MODE: error: option '-c' is not supported by 'swift'; did you mean to use 'swiftc'?
// RUN: not %swift_driver -### -emit-object %s 2>&1 | %FileCheck -check-prefix=OBJ_MODE %s
// OBJ_MODE: error: option '-emit-object' is not supported by 'swift'; did you mean to use 'swiftc'?
// RUN: not %swift_driver -### -emit-executable %s 2>&1 | %FileCheck -check-prefix=EXEC_MODE %s
// EXEC_MODE: error: option '-emit-executable' is not supported by 'swift'; did you mean to use 'swiftc'?
// RUN: not %swift_driver -### -o %t %s 2>&1 | %FileCheck -check-prefix=ARG_o %s
// ARG_o: error: option '-o' is not supported by 'swift'; did you mean to use 'swiftc'?

// RUN: not %swiftc_driver -### -repl 2>&1 | %FileCheck -check-prefix=REPL_MODE_SWIFTC %s
// REPL_MODE_SWIFTC: error: option '-repl' is not supported by 'swiftc'; did you mean to use 'swift'?
// RUN: not %swiftc_driver -### -lldb-repl 2>&1 | %FileCheck -check-prefix=LLDB_REPL_MODE_SWIFTC %s
// LLDB_REPL_MODE_SWIFTC: error: option '-lldb-repl' is not supported by 'swiftc'; did you mean to use 'swift'?
// RUN: not %swiftc_driver -### -deprecated-integrated-repl 2>&1 | %FileCheck -check-prefix=INT_REPL_MODE_SWIFTC %s
// INT_REPL_MODE_SWIFTC: error: option '-deprecated-integrated-repl' is not supported by 'swiftc'; did you mean to use 'swift'?

// RUN: %swift_driver -g -### %s 2>&1 | %FileCheck -check-prefix=OPTIONS_BEFORE_FILE %s
// OPTIONS_BEFORE_FILE: -g

// RUN: not %swiftc_driver_plain -target x86_64-unknown-hurd -### %s 2>&1 | %FileCheck -check-prefix=BAD_TARGET %s
// BAD_TARGET: error: unknown target 'x86_64-unknown-hurd'

// RUN: %swiftc_driver -driver-print-jobs -enforce-exclusivity=checked %s | %FileCheck -check-prefix=EXCLUSIVITY_CHECKED %s
// EXCLUSIVITY_CHECKED: swift
// EXCLUSIVITY_CHECKED: -enforce-exclusivity=checked

// RUN: %swiftc_driver -driver-print-jobs -remove-runtime-asserts %s | %FileCheck -check-prefix=REMOVE_RUNTIME_ASSERTS %s
// REMOVE_RUNTIME_ASSERTS: swift
// REMOVE_RUNTIME_ASSERTS: -frontend {{.*}} -remove-runtime-asserts

// RUN: %swiftc_driver -driver-print-jobs -assume-single-threaded %s | %FileCheck -check-prefix=ASSUME_SINGLE_THREADED %s
// ASSUME_SINGLE_THREADED: swift
// ASSUME_SINGLE_THREADED: -frontend {{.*}} -assume-single-threaded

// RUN: %swift_driver -### -g -debug-info-format=codeview %s | %FileCheck -check-prefix DEBUG_INFO_FORMAT_CODEVIEW %s
// RUN: %swift_driver -### -g -debug-info-format=dwarf %s | %FileCheck -check-prefix DEBUG_INFO_FORMAT_DWARF %s
// RUN: %swiftc_driver -### -g -debug-info-format=codeview %s | %FileCheck -check-prefix DEBUG_INFO_FORMAT_CODEVIEW %s
// RUN: %swiftc_driver -### -g -debug-info-format=dwarf %s | %FileCheck -check-prefix DEBUG_INFO_FORMAT_DWARF %s
// DEBUG_INFO_FORMAT_CODEVIEW: -debug-info-format=codeview
// DEBUG_INFO_FORMAT_DWARF: -debug-info-format=dwarf

// RUN: not %swift_driver -debug-info-format=dwarf %s 2>&1 | %FileCheck -check-prefix MISSING_OPTION_G_ERROR %s
// RUN: not %swift_driver -debug-info-format=codeview %s 2>&1 | %FileCheck -check-prefix MISSING_OPTION_G_ERROR %s
// RUN: not %swiftc_driver -debug-info-format=dwarf %s 2>&1 | %FileCheck -check-prefix MISSING_OPTION_G_ERROR %s
// RUN: not %swiftc_driver -debug-info-format=codeview %s 2>&1 | %FileCheck -check-prefix MISSING_OPTION_G_ERROR %s
// MISSING_OPTION_G_ERROR: error: option '-debug-info-format={{.*}}' is missing a required argument (-g)

// RUN: %swift_driver -### -g -dwarf-version=3 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_3 %s
// DWARF_VERSION_5: -dwarf-version=5
// DWARF_VERSION_4: -dwarf-version=4
// DWARF_VERSION_3: -dwarf-version=3
// DWARF_VERSION_2: -dwarf-version=2
// RUN: not %swift_driver -dwarf-version=1 %s 2>&1 | %FileCheck -check-prefix INVALID_DWARF_VERSION %s
// RUN: not %swift_driver -dwarf-version=6 %s 2>&1 | %FileCheck -check-prefix INVALID_DWARF_VERSION %s
// RUN: not %swiftc_driver -dwarf-version=1 %s 2>&1 | %FileCheck -check-prefix INVALID_DWARF_VERSION %s
// RUN: not %swiftc_driver -dwarf-version=6 %s 2>&1 | %FileCheck -check-prefix INVALID_DWARF_VERSION %s
// INVALID_DWARF_VERSION: invalid value '{{1|6}}' in '-dwarf-version={{1|6}}'

// RUN: %swift_driver -### -g -target x86_64-apple-macosx10.10 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_2 %s
// RUN: %swiftc_driver -### -g -target x86_64-apple-macosx10.10 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_2 %s
// RUN: %swift_driver -### -g -target x86_64-apple-macosx10.11 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_4 %s
// RUN: %swiftc_driver -### -g -target x86_64-apple-macosx10.11 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_4 %s
// RUN: %swift_driver -### -g -target x86_64-apple-macos14.0 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_4 %s
// RUN: %swiftc_driver -### -g -target x86_64-apple-macos14.0 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_4 %s
// RUN: %swift_driver -### -g -target arm64-apple-ios8.0 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_2 %s
// RUN: %swiftc_driver -### -g -target arm64-apple-ios8.0 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_2 %s
// RUN: %swift_driver -### -g -target arm64-apple-ios9.0 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_4 %s
// RUN: %swiftc_driver -### -g -target arm64-apple-ios9.0 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_4 %s
// RUN: %swift_driver -### -g -target x86_64-apple-ios17.0-macabi %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_4 %s
// RUN: %swiftc_driver -### -g -target x86_64-apple-ios17.0-macabi %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_4 %s
// RUN: %swift_driver -### -g -target arm64-apple-tvos17.0 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_4 %s
// RUN: %swiftc_driver -### -g -target arm64-apple-tvos17.0 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_4 %s
// RUN: %swift_driver -### -g -target arm64_32-apple-watchos10.0 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_4 %s
// RUN: %swiftc_driver -### -g -target arm64_32-apple-watchos10.0 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_4 %s

// RUN: %swift_driver -### -g -target x86_64-apple-macosx15 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_5 %s
// RUN: %swiftc_driver -### -g -target x86_64-apple-macosx15 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_5 %s
// RUN: %swift_driver -### -g -target arm64-apple-ios18.0 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_5 %s
// RUN: %swiftc_driver -### -g -target arm64-apple-ios18.0 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_5 %s
// RUN: %swift_driver -### -g -target arm64-apple-ios18.0-macabi %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_5 %s
// RUN: %swiftc_driver -### -g -target arm64-apple-ios18.0-macabi %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_5 %s
// RUN: %swift_driver -### -g -target arm64-apple-tvos18 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_5 %s
// RUN: %swiftc_driver -### -g -target arm64-apple-tvos18 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_5 %s
// RUN: %swift_driver -### -g -target arm64_32-apple-watchos11 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_5 %s
// RUN: %swiftc_driver -### -g -target arm64_32-apple-watchos11 %s 2>&1 | %FileCheck -check-prefix DWARF_VERSION_5 %s

// RUN: not %swift_driver -gline-tables-only -debug-info-format=codeview %s 2>&1 | %FileCheck -check-prefix BAD_DEBUG_LEVEL_ERROR %s
// RUN: not %swift_driver -gdwarf-types -debug-info-format=codeview %s 2>&1 | %FileCheck -check-prefix BAD_DEBUG_LEVEL_ERROR %s
// RUN: not %swiftc_driver -gline-tables-only -debug-info-format=codeview %s 2>&1 | %FileCheck -check-prefix BAD_DEBUG_LEVEL_ERROR %s
// RUN: not %swiftc_driver -gdwarf-types -debug-info-format=codeview %s 2>&1 | %FileCheck -check-prefix BAD_DEBUG_LEVEL_ERROR %s
// BAD_DEBUG_LEVEL_ERROR: error: argument '-debug-info-format=codeview' is not allowed with '{{.*}}'

// RUN: %swift_driver -F %t/test.framework %s 2>&1 | %FileCheck -check-prefix SEARCH_PATH_INCLUDES_FRAMEWORK_EXTENSION %s
// RUN: %target-swiftc_driver -F %t/test.framework %s 2>&1 | %FileCheck -check-prefix SEARCH_PATH_INCLUDES_FRAMEWORK_EXTENSION %s
// RUN: %swift_driver -Fsystem %t/test.framework %s 2>&1 | %FileCheck -check-prefix SEARCH_PATH_INCLUDES_FRAMEWORK_EXTENSION %s
// RUN: %target-swiftc_driver -Fsystem %t/test.framework %s 2>&1 | %FileCheck -check-prefix SEARCH_PATH_INCLUDES_FRAMEWORK_EXTENSION %s
// RUN: %swift_driver -F %t/test.framework/ %s 2>&1 | %FileCheck -check-prefix SEARCH_PATH_INCLUDES_FRAMEWORK_EXTENSION %s
// RUN: %target-swiftc_driver -F %t/test.framework/ %s 2>&1 | %FileCheck -check-prefix SEARCH_PATH_INCLUDES_FRAMEWORK_EXTENSION %s
// RUN: %swift_driver -Fsystem %t/test.framework/ %s 2>&1 | %FileCheck -check-prefix SEARCH_PATH_INCLUDES_FRAMEWORK_EXTENSION %s
// RUN: %target-swiftc_driver -Fsystem %t/test.framework/ %s 2>&1 | %FileCheck -check-prefix SEARCH_PATH_INCLUDES_FRAMEWORK_EXTENSION %s
// SEARCH_PATH_INCLUDES_FRAMEWORK_EXTENSION: warning: framework search path ends in ".framework"
