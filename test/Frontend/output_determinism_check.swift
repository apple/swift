// RUN: %empty-directory(%t)
// RUN: echo '[]' > %t/protocol.json
// RUN: %target-swift-frontend -module-name test -emit-module -o %t/test.swiftmodule %s -emit-module-doc-path %t/test.docc -const-gather-protocols-file %t/protocol.json -emit-const-values-path %t/test.swiftconstvalues -emit-tbd-path %t/test.tbd -tbd-current-version 1 -tbd-compatibility-version 1 -tbd-install_name @rpath/test.dylib -enable-deterministic-check 2>&1 | %FileCheck %s --check-prefix=MODULE_OUTPUT --check-prefix=DOCC_OUTPUT --check-prefix=CONSTVALUE_OUTPUT --check-prefix=TBD_OUTPUT
// RUN: %target-swift-frontend -module-name test -emit-sib -o %t/test.sib -primary-file %s -enable-deterministic-check 2>&1 | %FileCheck %s --check-prefix=SIB_OUTPUT

/// object files are "not" deterministic because the second run going to match the mod hash and skip code generation.
// RUN: not %target-swift-frontend -module-name test -c -o %t/test.o -primary-file %s -enable-deterministic-check 2>&1 | %FileCheck %s --check-prefix=OBJECT_MISMATCH
/// object files should match when forcing object generation.
// RUN: %target-swift-frontend -module-name test -emit-dependencies -c -o %t/test.o -primary-file %s -enable-deterministic-check -always-compile-output-files 2>&1 | %FileCheck %s --check-prefix=OBJECT_OUTPUT --check-prefix=DEPS_OUTPUT

/// FIXME: Fine-grain dependencies graph is not deterministics.
/// FAIL:  %target-swift-frontend -module-name test -emit-reference-dependencies-path %t/test.swiftdeps -c -o %t/test.o -primary-file %s -enable-deterministic-check -always-compile-output-files

/// Explicit module build. Check building swiftmodule from interface file.
// RUN: %target-swift-frontend -scan-dependencies -module-name test -o %t/test.json %s -enable-deterministic-check 2>&1 | %FileCheck %s --check-prefix=DEPSCAN_OUTPUT
/// TODO: Implicit module build use a different compiler instance so it doesn't support checking yet.
// RUN: %target-swift-frontend -typecheck -emit-module-interface-path %t/test.swiftinterface %s -O -enable-deterministic-check 2>&1 | %FileCheck %s --check-prefix=INTERFACE_OUTPUT
/// Hit cache and not emit the second time.
// RUN: rm %t/test.swiftmodule
// RUN: not %target-swift-frontend -compile-module-from-interface %t/test.swiftinterface -explicit-interface-module-build -o %t/test.swiftmodule -enable-deterministic-check 2>&1 | %FileCheck --check-prefix=MODULE_MISMATCH %s
/// Force swiftmodule generation.
// RUN: %target-swift-frontend -compile-module-from-interface %t/test.swiftinterface -explicit-interface-module-build -o %t/test.swiftmodule -enable-deterministic-check -always-compile-output-files 2>&1 | %FileCheck --check-prefix=MODULE_OUTPUT %s

// RUN: %target-swift-frontend -scan-dependencies -module-name test %s -o %t/test.deps.json -enable-deterministic-check 2>&1 | %FileCheck %s --check-prefix=DEPS_JSON_OUTPUT

// RUN: %target-swift-frontend -emit-pcm -module-name UserClangModule -o %t/test.pcm %S/Inputs/dependencies/module.modulemap -enable-deterministic-check 2>&1 | %FileCheck %s --check-prefix=PCM_OUTPUT

// DOCC_OUTPUT: remark: produced matching output file '{{.*}}{{/|\\}}test.docc'
// CONSTVALUE_OUTPUT: remark: produced matching output file '{{.*}}{{/|\\}}test.swiftconstvalues'
// MODULE_OUTPUT: remark: produced matching output file '{{.*}}{{/|\\}}test.swiftmodule'
// TBD_OUTPUT: remark: produced matching output file '{{.*}}{{/|\\}}test.tbd'
// SIB_OUTPUT: remark: produced matching output file '{{.*}}{{/|\\}}test.sib'
// DEPS_OUTPUT: remark: produced matching output file '{{.*}}{{/|\\}}test.d'
// OBJECT_OUTPUT: remark: produced matching output file '{{.*}}{{/|\\}}test.o'
// OBJECT_MISMATCH: error: output file '{{.*}}{{/|\\}}test.o' is missing from second compilation for deterministic check
// DEPSCAN_OUTPUT: remark: produced matching output file '{{.*}}{{/|\\}}test.json'
// INTERFACE_OUTPUT: remark: produced matching output file '{{.*}}{{/|\\}}test.swiftinterface'
// MODULE_MISMATCH: error: output file '{{.*}}{{/|\\}}test.swiftmodule' is missing from second compilation for deterministic check
// DEPS_JSON_OUTPUT: remark: produced matching output file '{{.*}}{{/|\\}}test.deps.json'
// PCM_OUTPUT: remark: produced matching output file '{{.*}}{{/|\\}}test.pcm'

public var x = 1
public func test() {}
