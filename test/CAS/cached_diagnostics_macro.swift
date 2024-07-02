// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)
// RUN: split-file %s %t
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/%target-library-name(MacroDefinition) -module-name=MacroDefinition %t/macro.swift

// RUN: %target-swift-frontend -typecheck -module-load-mode prefer-serialized -module-name MyApp -module-cache-path %t/clang-module-cache -O \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import \
// RUN:   %t/main.swift -swift-version 5 -external-plugin-path %t#%swift-plugin-server 2> %t/diag1.txt

// RUN: %target-swift-frontend -scan-dependencies -module-load-mode prefer-serialized -module-name MyApp -module-cache-path %t/clang-module-cache -O \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import \
// RUN:   %t/main.swift -o %t/deps.json -swift-version 5 -cache-compile-job -cas-path %t/cas -external-plugin-path %t#%swift-plugin-server

// RUN: %S/Inputs/SwiftDepsExtractor.py %t/deps.json MyApp casFSRootID > %t/fs.casid
// RUN: llvm-cas -cas %t/cas -ls-tree-recursive @%t/fs.casid | %FileCheck %s --check-prefix=FS

// FS: MacroDefinition
// RUN: %S/Inputs/BuildCommandExtractor.py %t/deps.json clang:SwiftShims > %t/SwiftShims.cmd
// RUN: %swift_frontend_plain @%t/SwiftShims.cmd

// RUN: %S/Inputs/BuildCommandExtractor.py %t/deps.json MyApp > %t/MyApp.cmd
// RUN: %{python} %S/Inputs/GenerateExplicitModuleMap.py %t/deps.json > %t/map.json
// RUN: llvm-cas --cas %t/cas --make-blob --data %t/map.json > %t/map.casid

// RUN: %target-swift-frontend -diagnostic-style=swift \
// RUN:   -emit-module -o %t/Test.swiftmodule -cache-compile-job -cas-path %t/cas \
// RUN:   -swift-version 5 -disable-implicit-swift-modules \
// RUN:   -external-plugin-path %t#%swift-plugin-server \
// RUN:   -disable-implicit-string-processing-module-import -disable-implicit-concurrency-module-import \
// RUN:   -module-name MyApp -explicit-swift-module-map-file @%t/map.casid -O \
// RUN:   %t/main.swift @%t/MyApp.cmd -serialize-diagnostics-path %t/Test.diag 2> %t/diag2.txt

// RUN: diff %t/diag1.txt %t/diag2.txt

//--- macro.swift
import SwiftDiagnostics
import SwiftOperators
import SwiftSyntax
import SwiftSyntaxBuilder
@_spi(ExperimentalLanguageFeature) import SwiftSyntaxMacros

public struct CallDeprecatedMacro: ExpressionMacro {
   public static func expansion(
     of macro: some FreestandingMacroExpansionSyntax,
     in context: some MacroExpansionContext
   ) throws -> ExprSyntax {
     return "testDeprecated()"
  }
}

//--- main.swift
@freestanding(expression) macro myWarning(_ message: String) = #externalMacro(module: "MacroDefinition", type: "CallDeprecatedMacro")

@available(*, deprecated)
func testDeprecated() {}

func testDiscardableStringify(x: Int) {
  #myWarning("this is a warning")
}
