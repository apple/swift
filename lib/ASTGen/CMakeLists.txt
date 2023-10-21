add_pure_swift_host_library(swiftLLVMJSON STATIC EMIT_MODULE
  Sources/LLVMJSON/LLVMJSON.swift

  DEPENDENCIES
    swiftBasic
)

add_pure_swift_host_library(swiftASTGen STATIC
  Sources/ASTGen/ASTGen.swift
  Sources/ASTGen/Bridge.swift
  Sources/ASTGen/Decls.swift
  Sources/ASTGen/Diagnostics.swift
  Sources/ASTGen/DiagnosticsBridge.swift
  Sources/ASTGen/Exprs.swift
  Sources/ASTGen/Generics.swift
  Sources/ASTGen/Literals.swift
  Sources/ASTGen/Macros.swift
  Sources/ASTGen/ParameterClause.swift
  Sources/ASTGen/PluginHost.swift
  Sources/ASTGen/SourceFile.swift
  Sources/ASTGen/SourceManager.swift
  Sources/ASTGen/SourceManager+MacroExpansionContext.swift
  Sources/ASTGen/Stmts.swift
  Sources/ASTGen/Types.swift

  DEPENDENCIES
    swiftAST
  SWIFT_DEPENDENCIES
    SwiftBasicFormat
    SwiftCompilerPluginMessageHandling
    SwiftDiagnostics
    SwiftOperators
    SwiftParser
    SwiftParserDiagnostics
    SwiftSyntax
    SwiftSyntaxBuilder
    SwiftSyntaxMacros
    SwiftSyntaxMacroExpansion
    swiftLLVMJSON
)

# Note: This is copy pasted from SwiftCompilerSources.
set(c_include_paths
  # LLVM modules and headers.
  "${LLVM_MAIN_INCLUDE_DIR}"
  # Generated LLVM headers.
  "${LLVM_INCLUDE_DIR}"
  # Clang modules and headers.
  ${CLANG_INCLUDE_DIRS}
  # Bridging modules and headers.
  "${SWIFT_MAIN_INCLUDE_DIR}"
  # Generated C headers.
  "${CMAKE_CURRENT_BINARY_DIR}/../../include")
set(c_include_paths_args)
foreach(c_include_path ${c_include_paths})
  list(APPEND c_include_paths_args "SHELL: -Xcc -I -Xcc ${c_include_path}")
endforeach()

target_compile_options(swiftASTGen PRIVATE
  ${c_include_paths_args}
  "SHELL: -Xfrontend -enable-experimental-cxx-interop"
  "SHELL: -Xcc -std=c++17"
  "SHELL: -Xcc -DIMPORTING_INTO_SWIFT"
  "SHELL: -Xcc -DIMPORTING_INTO_ASTGEN"

  # Necessary to avoid treating IBOutlet and IBAction as keywords
  "SHELL:-Xcc -UIBOutlet -Xcc -UIBAction -Xcc -UIBInspectable"
)
