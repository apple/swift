// RUN: %empty-directory(%t)
// RUN: cp -r %S/Inputs/EmitWhileBuilding/EmitWhileBuilding.framework %t
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -enable-objc-interop -emit-module-path %t/EmitWhileBuilding.framework/Modules/EmitWhileBuilding.swiftmodule/%target-swiftmodule-name -import-underlying-module -F %t -module-name EmitWhileBuilding -disable-objc-attr-requires-foundation-module %s %S/Inputs/EmitWhileBuilding/Extra.swift -emit-symbol-graph -emit-symbol-graph-dir %t -symbol-graph-minimum-access-level internal
// RUN: %FileCheck %s --input-file %t/EmitWhileBuilding.symbols.json
// RUN: %{python} -c 'import os.path; import sys; sys.exit(1 if os.path.exists(sys.argv[1]) else 0)' %t/EmitWhileBuilding@EmitWhileBuilding.symbols.json

// RUN: %target-swift-symbolgraph-extract -sdk %clang-importer-sdk -module-name EmitWhileBuilding -F %t -output-dir %t -pretty-print -v -minimum-access-level internal
// RUN: %FileCheck %s --input-file %t/EmitWhileBuilding.symbols.json
// RUN: %{python} -c 'import os.path; import sys; sys.exit(1 if os.path.exists(sys.argv[1]) else 0)' %t/EmitWhileBuilding@EmitWhileBuilding.symbols.json

// REQUIRES: objc_interop

// Ensure that having an underlying Clang module does not override the
// `-symbol-graph-minimum-access-level` flag (rdar://110399757)

// CHECK: "s:17EmitWhileBuilding9innerFuncSSyF"

internal func innerFunc() -> String { "sup" }

public func someFunc() -> String { innerFunc() }
