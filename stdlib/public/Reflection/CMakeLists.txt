set(swiftReflection_SOURCES
  MetadataSource.cpp
  TypeLowering.cpp
  TypeRef.cpp
  TypeRefBuilder.cpp
  "${SWIFT_SOURCE_DIR}/stdlib/public/runtime/LLVMSupport.cpp"
  "${SWIFT_SOURCE_DIR}/lib/Demangling/Context.cpp"
  "${SWIFT_SOURCE_DIR}/lib/Demangling/OldDemangler.cpp"
  "${SWIFT_SOURCE_DIR}/lib/Demangling/Demangler.cpp"
  "${SWIFT_SOURCE_DIR}/lib/Demangling/NodePrinter.cpp"
  "${SWIFT_SOURCE_DIR}/lib/Demangling/ManglingUtils.cpp"
  "${SWIFT_SOURCE_DIR}/lib/Demangling/Punycode.cpp"
  "${SWIFT_SOURCE_DIR}/lib/Demangling/Remangler.cpp")

# When we're building with assertions, include the demangle node dumper to aid
# in debugging.
if (LLVM_ENABLE_ASSERTIONS)
  list(APPEND swiftReflection_SOURCES
    "${SWIFT_SOURCE_DIR}/lib/Demangling/NodeDumper.cpp")
endif(LLVM_ENABLE_ASSERTIONS)

if(SWIFT_BUILD_STDLIB)
  add_swift_target_library(swiftReflection STATIC
    ${swiftReflection_SOURCES}
    C_COMPILE_FLAGS ${SWIFT_RUNTIME_CXX_FLAGS} -DswiftCore_EXPORTS
    LINK_FLAGS ${SWIFT_RUNTIME_LINK_FLAGS}
    SWIFT_COMPILE_FLAGS ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
    INSTALL_IN_COMPONENT dev)
endif()

# Build a specific version for the host with the host toolchain.  This is going
# to be used by tools (e.g. lldb)
if(SWIFT_BUILD_TOOLS)
  if(NOT SWIFT_BUILD_STDLIB)
    add_custom_target(swiftReflection-${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR})
  endif()

  if(NOT SWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER)
    set(CURRENT_CMAKE_C_COMPILER ${CMAKE_C_COMPILER})
    set(CURRENT_CMAKE_CXX_COMPILER ${CMAKE_CXX_COMPILER})
    set(CMAKE_C_COMPILER ${HOST_CMAKE_C_COMPILER})
    set(CMAKE_CXX_COMPILER ${HOST_CMAKE_CXX_COMPILER})
  endif()

  add_swift_host_library(swiftReflection STATIC
    ${swiftReflection_SOURCES})
  target_compile_options(swiftReflection PRIVATE
    ${SWIFT_RUNTIME_CXX_FLAGS})
  set_property(TARGET swiftReflection
    APPEND_STRING PROPERTY LINK_FLAGS ${SWIFT_RUNTIME_LINK_FLAGS})

  if(NOT SWIFT_BUILD_RUNTIME_WITH_HOST_COMPILER)
    set(CMAKE_C_COMPILER ${CURRENT_CMAKE_C_COMPILER})
    set(CMAKE_CXX_COMPILER ${CURRENT_CMAKE_CXX_COMPILER})
  endif()
endif()
