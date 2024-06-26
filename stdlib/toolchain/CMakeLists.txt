# Toolchain-only build products

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/../cmake/modules)
include(StdlibOptions)
include(AddSwiftStdlib)
include(CompatibilityLibs)

set(CXX_COMPILE_FLAGS)
set(CXX_LINK_FLAGS)


# Build the runtime with -Wall to catch, e.g., uninitialized variables
# warnings.
if(SWIFT_COMPILER_IS_MSVC_LIKE)
  list(APPEND compile_flags "/W3")
else()
  list(APPEND compile_flags "-Wall")
endif()

list(APPEND compile_flags
  # C++ code in the runtime and standard library should generally avoid
  # introducing static constructors or destructors.
  "-Wglobal-constructors"
  "-Wexit-time-destructors")

foreach(flag ${compile_flags})
  check_cxx_compiler_flag("${flag}" cxx_compiler_flag_${flag}_is_supported)
  if(cxx_compiler_flag_${flag}_is_supported)
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

set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN YES)

macro(dont_target_arm64e_for_products_configured_in_this_folder)
  # arm64e is not ABI stable and not supported in opensource
  # This is meant to be used inside the CMakeLists.txt for each backdeployed product
  # so that the effect are local to those files only
  # https://cmake.org/cmake/help/latest/manual/cmake-language.7.html#variables
  # This is defined as a macro on purpose so that the effect applies to the caller,
  # and not inside the scope of a function
  list_intersect("${SWIFT_DARWIN_PLATFORMS}" "${SWIFT_SDKS}" building_darwin_sdks)
  foreach(sdk ${building_darwin_sdks})
    list(REMOVE_ITEM SWIFT_SDK_${sdk}_ARCHITECTURES "arm64e")
  endforeach()
endmacro()

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
  get_compatibility_libs(
    "${SWIFT_HOST_VARIANT_SDK}"
    "${SWIFT_HOST_VARIANT_ARCH}"
    compatibility_libs
    )
  target_link_libraries(HostCompatibilityLibs INTERFACE
    ${compatibility_libs}
    )
  set_property(GLOBAL APPEND PROPERTY SWIFT_BUILDTREE_EXPORTS HostCompatibilityLibs)
endif()
