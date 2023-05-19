# Toolchain-only build products

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/../cmake/modules)
include(StdlibOptions)
include(AddSwiftStdlib)

set(CXX_COMPILE_FLAGS)
set(CXX_LINK_FLAGS)


set(compile_flags
  # Build the runtime with -Wall to catch, e.g., uninitialized variables
  # warnings.
  "-Wall"

  # C++ code in the runtime and standard library should generally avoid
  # introducing static constructors or destructors.
  "-Wglobal-constructors"
  "-Wexit-time-destructors")


# Build the runtime with -Wall to catch, e.g., uninitialized variables
# warnings.
if(SWIFT_COMPILER_IS_MSVC_LIKE)
  list(APPEND compile_flags "/W3")
else()
  list(APPEND compile_flags "-Wall")
endif()


foreach(flag ${compile_flags})
  check_cxx_compiler_flag("${flag}" is_supported)
  if(is_supported)
    list(APPEND CXX_COMPILE_FLAGS "${flag}")
  endif()
endforeach()
unset(compile_flags)


if("Thread" IN_LIST SWIFT_RUNTIME_USE_SANITIZERS)
  list(APPEND CXX_LINK_FLAGS "-fsanitize=thread")
endif()

# Compatibility libraries build in a special alternate universe that can't
# directly link to most OS runtime libraries, and have to access the
# runtime being patched only through public ABI.
list(APPEND CXX_COMPILE_FLAGS "-DSWIFT_COMPATIBILITY_LIBRARY=1")

if(SWIFT_STDLIB_SUPPORT_BACK_DEPLOYMENT)
  add_subdirectory(legacy_layouts)
  add_subdirectory(Compatibility50)
  add_subdirectory(Compatibility51)
  add_subdirectory(Compatibility56)
  add_subdirectory(CompatibilityDynamicReplacements)
  add_subdirectory(CompatibilityConcurrency)
  add_subdirectory(CompatibilityThreading)
  add_subdirectory(CompatibilityPacks)

  # This is a convenience target to have the list
  # of all the compatibility libraries needed to build
  # host tools in a single place
  add_library(HostCompatibilityLibs INTERFACE)
  set(vsuffix "-${SWIFT_SDK_${SWIFT_HOST_VARIANT_SDK}_LIB_SUBDIR}-${SWIFT_HOST_VARIANT_ARCH}")
  target_link_libraries(HostCompatibilityLibs INTERFACE
    swiftCompatibilityConcurrency${vsuffix}
    swiftCompatibilityDynamicReplacements${vsuffix}
    swiftCompatibilityPacks${vsuffix}
    swiftCompatibility50${vsuffix}
    swiftCompatibility51${vsuffix}
    swiftCompatibility56${vsuffix})
  set_property(GLOBAL APPEND PROPERTY SWIFT_BUILDTREE_EXPORTS HostCompatibilityLibs)
endif()
