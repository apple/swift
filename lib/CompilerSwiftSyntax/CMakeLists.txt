if(NOT SWIFT_BUILD_SWIFT_SYNTAX)
  return()
endif()
if(NOT EXISTS "${SWIFT_PATH_TO_SWIFT_SYNTAX_SOURCE}")
  message(SEND_ERROR "swift-syntax is required to build the Swift compiler. Please run update-checkout or specify SWIFT_PATH_TO_SWIFT_SYNTAX_SOURCE")
  return()
endif()

# Build swift-syntax libraries with FetchContent.
function(includeSwiftSyntax)
  set(CMAKE_Swift_COMPILER_TARGET ${SWIFT_HOST_TRIPLE})
  set(BUILD_SHARED_LIBS ON)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${SWIFT_HOST_LIBRARIES_DEST_DIR}/compiler")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${SWIFT_HOST_LIBRARIES_DEST_DIR}/compiler")
  if(SWIFT_HOST_VARIANT_SDK MATCHES "LINUX|ANDROID|OPENBSD|FREEBSD")
    set(SWIFT_HOST_LIBRARIES_RPATH "$ORIGIN;$ORIGIN/../../${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR}")
  endif()

  # Add unique ABI prefix to swift-syntax libraries so that compiler libraries (e.g. sourcekitdInProc)
  # can be used from tools that has its own swift-syntax libraries as SwiftPM dependencies.
  set(SWIFT_MODULE_ABI_NAME_PREFIX "_Compiler")
  set(SWIFTSYNTAX_TARGET_NAMESPACE "_Compiler")
  set(SWIFTSYNTAX_EMIT_MODULE OFF)

  file(TO_CMAKE_PATH "${SWIFT_PATH_TO_SWIFT_SYNTAX_SOURCE}" swift_syntax_path)
  FetchContent_Declare(CompilerSwiftSyntax SOURCE_DIR "${swift_syntax_path}")
  FetchContent_MakeAvailable(CompilerSwiftSyntax)
endfunction()
includeSwiftSyntax()

set(compiler_swiftsyntax_libs
  _CompilerSwiftSyntax
  _CompilerSwiftOperators
  _CompilerSwiftSyntaxBuilder
  _CompilerSwiftParser
  _CompilerSwiftParserDiagnostics
  _CompilerSwiftCompilerPluginMessageHandling
  _CompilerSwiftSyntaxMacroExpansion
  _CompilerSwiftSyntaxMacros
  _CompilerSwiftBasicFormat
  _CompilerSwiftDiagnostics
  _CompilerSwiftIDEUtils
)

foreach(lib ${compiler_swiftsyntax_libs})
  target_compile_options(${lib} PRIVATE "SHELL:-module-link-name ${lib}")
endforeach()

swift_install_in_component(TARGETS ${compiler_swiftsyntax_libs}
  ARCHIVE DESTINATION "lib${LLVM_LIBDIR_SUFFIX}/swift/host/compiler" COMPONENT compiler
  LIBRARY DESTINATION "lib${LLVM_LIBDIR_SUFFIX}/swift/host/compiler" COMPONENT compiler
  RUNTIME DESTINATION "bin" COMPONENT compiler)
