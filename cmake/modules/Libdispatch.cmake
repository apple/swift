include(ExternalProject)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND
    CMAKE_C_COMPILER_VERSION VERSION_GREATER 3.8
    OR LLVM_USE_SANITIZER)
    set(SWIFT_LIBDISPATCH_C_COMPILER ${CMAKE_C_COMPILER})
    set(SWIFT_LIBDISPATCH_CXX_COMPILER ${CMAKE_CXX_COMPILER})
  elseif(CMAKE_SYSTEM_NAME STREQUAL CMAKE_HOST_SYSTEM_NAME)
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
      if(CMAKE_SYSTEM_PROCESSOR STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR AND
        TARGET clang)
        set(SWIFT_LIBDISPATCH_C_COMPILER
          $<TARGET_FILE_DIR:clang>/clang-cl${CMAKE_EXECUTABLE_SUFFIX})
        set(SWIFT_LIBDISPATCH_CXX_COMPILER
          $<TARGET_FILE_DIR:clang>/clang-cl${CMAKE_EXECUTABLE_SUFFIX})
      elseif(DEFINED SWIFT_CLANG_LOCATION)
        set(SWIFT_LIBDISPATCH_C_COMPILER ${SWIFT_CLANG_LOCATION}/clang-cl${CMAKE_EXECUTABLE_SUFFIX})
        set(SWIFT_LIBDISPATCH_CXX_COMPILER ${SWIFT_CLANG_LOCATION}/clang-cl${CMAKE_EXECUTABLE_SUFFIX})
      else()
        set(SWIFT_LIBDISPATCH_C_COMPILER clang-cl${CMAKE_EXECUTABLE_SUFFIX})
        set(SWIFT_LIBDISPATCH_CXX_COMPILER clang-cl${CMAKE_EXECUTABLE_SUFFIX})
      endif()
    else()
      set(SWIFT_LIBDISPATCH_C_COMPILER $<TARGET_FILE_DIR:clang>/clang)
      set(SWIFT_LIBDISPATCH_CXX_COMPILER $<TARGET_FILE_DIR:clang>/clang++)
    endif()
  else()
    message(SEND_ERROR "libdispatch requires a newer clang compiler (${CMAKE_C_COMPILER_VERSION} < 3.9)")
  endif()

  if(SWIFT_HOST_VARIANT_SDK STREQUAL "WINDOWS")
    set(LIBDISPATCH_RUNTIME_DIR bin)
  else()
    set(LIBDISPATCH_RUNTIME_DIR lib)
  endif()
endif()

set(DISPATCH_SDKS)

# Build the host libdispatch if needed.
if(SWIFT_BUILD_HOST_DISPATCH)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    if(NOT "${SWIFT_HOST_VARIANT_SDK}" IN_LIST SWIFT_SDKS)
      list(APPEND DISPATCH_SDKS "${SWIFT_HOST_VARIANT_SDK}")
    endif()
  endif()
endif()

# Build any target libdispatch if needed.
foreach(sdk ${SWIFT_SDKS})
  # Darwin targets have libdispatch available, do not build it.
  # Wasm/WASI doesn't support libdispatch yet.
  # See https://github.com/apple/swift/issues/54533 for more details.
  if(NOT "${sdk}" IN_LIST SWIFT_DARWIN_PLATFORMS AND NOT "${sdk}" STREQUAL "WASI")
    list(APPEND DISPATCH_SDKS "${sdk}")
  endif()
endforeach()

foreach(sdk ${DISPATCH_SDKS})
  set(ARCHS ${SWIFT_SDK_${sdk}_ARCHITECTURES})
  if(sdk STREQUAL "${SWIFT_HOST_VARIANT_SDK}")
    if(NOT "${SWIFT_HOST_VARIANT_ARCH}" IN_LIST ARCHS)
      list(APPEND ARCHS "${SWIFT_HOST_VARIANT_ARCH}")
    endif()
  endif()
  
  if(sdk STREQUAL "ANDROID")
    set(SWIFT_LIBDISPATCH_COMPILER_CMAKE_ARGS)
  else()
    set(SWIFT_LIBDISPATCH_COMPILER_CMAKE_ARGS -DCMAKE_C_COMPILER=${SWIFT_LIBDISPATCH_C_COMPILER};-DCMAKE_CXX_COMPILER=${SWIFT_LIBDISPATCH_CXX_COMPILER})
  endif()

  foreach(arch ${ARCHS})
    set(LIBDISPATCH_VARIANT_NAME "libdispatch-${SWIFT_SDK_${sdk}_LIB_SUBDIR}-${arch}")

    if(sdk MATCHES WINDOWS)
      set(SWIFT_LIBDISPATCH_COMPILER_TRIPLE_CMAKE_ARGS -DCMAKE_C_COMPILER_TARGET=${SWIFT_SDK_WINDOWS_ARCH_${arch}_TRIPLE};-DCMAKE_CXX_COMPILER_TARGET=${SWIFT_SDK_WINDOWS_ARCH_${arch}_TRIPLE})
    endif()

    if("${sdk}" STREQUAL "ANDROID")
      file(TO_CMAKE_PATH "${SWIFT_ANDROID_NDK_PATH}" _ANDROID_NDK)
      set(SWIFT_LIBDISPATCH_ANDROID_NDK -DCMAKE_ANDROID_NDK=${_ANDROID_NDK})
    else()
      set(SWIFT_LIBDISPATCH_SYSTEM_PROCESSOR  -DCMAKE_SYSTEM_PROCESSOR=${arch})
    endif()

    ExternalProject_Add("${LIBDISPATCH_VARIANT_NAME}"
                        SOURCE_DIR
                          "${SWIFT_PATH_TO_LIBDISPATCH_SOURCE}"
                        CMAKE_ARGS
                          -DCMAKE_AR=${CMAKE_AR}
                          -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
                          ${SWIFT_LIBDISPATCH_COMPILER_CMAKE_ARGS}
                          ${SWIFT_LIBDISPATCH_COMPILER_TRIPLE_CMAKE_ARGS}
                          -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
                          -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
                          -DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}
                          -DCMAKE_SHARED_LINKER_FLAGS=${CMAKE_SHARED_LINKER_FLAGS}
                          -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
                          -DCMAKE_INSTALL_LIBDIR=lib
                          -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
                          -DCMAKE_LINKER=${CMAKE_LINKER}
                          -DCMAKE_MT=${CMAKE_MT}
                          -DCMAKE_RANLIB=${CMAKE_RANLIB}
                          -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
                          -DCMAKE_SYSTEM_NAME=${SWIFT_SDK_${sdk}_NAME}
                          ${SWIFT_LIBDISPATCH_SYSTEM_PROCESSOR}
                          "${SWIFT_LIBDISPATCH_ANDROID_NDK}"
                          -DCMAKE_ANDROID_ARCH_ABI=${SWIFT_SDK_ANDROID_ARCH_${arch}_ABI}
                          -DCMAKE_ANDROID_API=${SWIFT_ANDROID_API_LEVEL}
                          -DBUILD_SHARED_LIBS=YES
                          -DENABLE_SWIFT=NO
                          -DBUILD_TESTING=NO
                        INSTALL_COMMAND
                          # NOTE(compnerd) provide a custom install command to
                          # ensure that we strip out the DESTDIR environment
                          # from the sub-build
                          ${CMAKE_COMMAND} -E env --unset=DESTDIR ${CMAKE_COMMAND} --build . --target install
                        COMMAND
                          ${CMAKE_COMMAND} -E copy
                          <INSTALL_DIR>/${LIBDISPATCH_RUNTIME_DIR}/${SWIFT_SDK_${sdk}_SHARED_LIBRARY_PREFIX}dispatch${SWIFT_SDK_${sdk}_SHARED_LIBRARY_SUFFIX}
                          ${SWIFTLIB_DIR}/${SWIFT_SDK_${sdk}_LIB_SUBDIR}/${arch}/${SWIFT_SDK_${sdk}_SHARED_LIBRARY_PREFIX}dispatch${SWIFT_SDK_${sdk}_SHARED_LIBRARY_SUFFIX}
                        COMMAND
                          ${CMAKE_COMMAND} -E copy
                          <INSTALL_DIR>/${LIBDISPATCH_RUNTIME_DIR}/${SWIFT_SDK_${sdk}_SHARED_LIBRARY_PREFIX}BlocksRuntime${SWIFT_SDK_${sdk}_SHARED_LIBRARY_SUFFIX}
                          ${SWIFTLIB_DIR}/${SWIFT_SDK_${sdk}_LIB_SUBDIR}/${arch}/${SWIFT_SDK_${sdk}_SHARED_LIBRARY_PREFIX}BlocksRuntime${SWIFT_SDK_${sdk}_SHARED_LIBRARY_SUFFIX}
                        COMMAND
                          ${CMAKE_COMMAND} -E copy
                          <INSTALL_DIR>/${LIBDISPATCH_RUNTIME_DIR}/${SWIFT_SDK_${sdk}_SHARED_LIBRARY_PREFIX}dispatch${SWIFT_SDK_${sdk}_SHARED_LIBRARY_SUFFIX}
                          ${SWIFTLIB_DIR}/${SWIFT_SDK_${sdk}_LIB_SUBDIR}/${SWIFT_SDK_${sdk}_SHARED_LIBRARY_PREFIX}dispatch${SWIFT_SDK_${sdk}_SHARED_LIBRARY_SUFFIX}
                        COMMAND
			  ${CMAKE_COMMAND} -E copy
                          <INSTALL_DIR>/${LIBDISPATCH_RUNTIME_DIR}/${SWIFT_SDK_${sdk}_SHARED_LIBRARY_PREFIX}BlocksRuntime${SWIFT_SDK_${sdk}_SHARED_LIBRARY_SUFFIX}
                          ${SWIFTLIB_DIR}/${SWIFT_SDK_${sdk}_LIB_SUBDIR}/${SWIFT_SDK_${sdk}_SHARED_LIBRARY_PREFIX}BlocksRuntime${SWIFT_SDK_${sdk}_SHARED_LIBRARY_SUFFIX}
                        STEP_TARGETS
                          install
                        BUILD_BYPRODUCTS
                          <INSTALL_DIR>/${LIBDISPATCH_RUNTIME_DIR}/${SWIFT_SDK_${sdk}_SHARED_LIBRARY_PREFIX}dispatch${SWIFT_SDK_${sdk}_SHARED_LIBRARY_SUFFIX}
                          <INSTALL_DIR>/lib/${SWIFT_SDK_${sdk}_IMPORT_LIBRARY_PREFIX}dispatch${SWIFT_SDK_${sdk}_IMPORT_LIBRARY_SUFFIX}
                          <INSTALL_DIR>/${LIBDISPATCH_RUNTIME_DIR}/${SWIFT_SDK_${sdk}_SHARED_LIBRARY_PREFIX}BlocksRuntime${SWIFT_SDK_${sdk}_SHARED_LIBRARY_SUFFIX}
                          <INSTALL_DIR>/lib/${SWIFT_SDK_${sdk}_IMPORT_LIBRARY_PREFIX}BlocksRuntime${SWIFT_SDK_${sdk}_IMPORT_LIBRARY_SUFFIX}
                        BUILD_ALWAYS
                          1)

    ExternalProject_Get_Property("${LIBDISPATCH_VARIANT_NAME}" install_dir)

    # CMake does not like the addition of INTERFACE_INCLUDE_DIRECTORIES without
    # the directory existing.  Just create the location which will be populated
    # during the installation.
    file(MAKE_DIRECTORY ${install_dir}/include)

    set(DISPATCH_VARIANT_NAME "dispatch-${SWIFT_SDK_${sdk}_LIB_SUBDIR}-${arch}")
    add_library("${DISPATCH_VARIANT_NAME}" SHARED IMPORTED GLOBAL)
    set_target_properties("${DISPATCH_VARIANT_NAME}"
                          PROPERTIES
                            IMPORTED_LOCATION
                              ${install_dir}/${LIBDISPATCH_RUNTIME_DIR}/${SWIFT_SDK_${sdk}_SHARED_LIBRARY_PREFIX}dispatch${SWIFT_SDK_${sdk}_SHARED_LIBRARY_SUFFIX}
                            IMPORTED_IMPLIB
                              ${install_dir}/lib/${SWIFT_SDK_${sdk}_IMPORT_LIBRARY_PREFIX}dispatch${SWIFT_SDK_${sdk}_IMPORT_LIBRARY_SUFFIX}
                            INTERFACE_INCLUDE_DIRECTORIES
                              ${install_dir}/include
                            IMPORTED_NO_SONAME
                              1)

    set(BLOCKS_RUNTIME_VARIANT_NAME "BlocksRuntime-${SWIFT_SDK_${sdk}_LIB_SUBDIR}-${arch}")
    add_library("${BLOCKS_RUNTIME_VARIANT_NAME}" SHARED IMPORTED GLOBAL)
    set_target_properties("${BLOCKS_RUNTIME_VARIANT_NAME}"
                          PROPERTIES
                            IMPORTED_LOCATION
                              ${install_dir}/${LIBDISPATCH_RUNTIME_DIR}/${SWIFT_SDK_${sdk}_SHARED_LIBRARY_PREFIX}BlocksRuntime${SWIFT_SDK_${sdk}_SHARED_LIBRARY_SUFFIX}
                            IMPORTED_IMPLIB
                              ${install_dir}/lib/${SWIFT_SDK_${sdk}_IMPORT_LIBRARY_PREFIX}BlocksRuntime${SWIFT_SDK_${sdk}_IMPORT_LIBRARY_SUFFIX}
                            INTERFACE_INCLUDE_DIRECTORIES
                              ${SWIFT_PATH_TO_LIBDISPATCH_SOURCE}/src/BlocksRuntime
                            IMPORTED_NO_SONAME
                              1)

    add_dependencies("${DISPATCH_VARIANT_NAME}" "${LIBDISPATCH_VARIANT_NAME}-install")
    add_dependencies("${BLOCKS_RUNTIME_VARIANT_NAME}" "${LIBDISPATCH_VARIANT_NAME}-install")

    if(SWIFT_BUILD_STATIC_STDLIB)
      set(LIBDISPATCH_STATIC_VARIANT_NAME "libdispatch-${SWIFT_SDK_${sdk}_LIB_SUBDIR}-${arch}-static")
      ExternalProject_Add("${LIBDISPATCH_STATIC_VARIANT_NAME}"
                          SOURCE_DIR
                            "${SWIFT_PATH_TO_LIBDISPATCH_SOURCE}"
                          CMAKE_ARGS
                            -DCMAKE_AR=${CMAKE_AR}
                            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
                            ${SWIFT_LIBDISPATCH_COMPILER_CMAKE_ARGS}
                            -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
                            -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}
                            -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
                            -DCMAKE_INSTALL_LIBDIR=lib
                            -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
                            -DCMAKE_LINKER=${CMAKE_LINKER}
                            -DCMAKE_RANLIB=${CMAKE_RANLIB}
                            -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
                            -DCMAKE_SYSTEM_NAME=${SWIFT_SDK_${sdk}_NAME}
                            ${SWIFT_LIBDISPATCH_SYSTEM_PROCESSOR}
                            "-DCMAKE_ANDROID_NDK=${SWIFT_ANDROID_NDK_PATH}"
                            -DCMAKE_ANDROID_ARCH_ABI=${SWIFT_SDK_ANDROID_ARCH_${arch}_ABI}
                            -DCMAKE_ANDROID_API=${SWIFT_ANDROID_API_LEVEL}
                            -DBUILD_SHARED_LIBS=NO
                            -DENABLE_SWIFT=NO
                            -DBUILD_TESTING=NO
                          INSTALL_COMMAND
                            # NOTE(compnerd) provide a custom install command to
                            # ensure that we strip out the DESTDIR environment
                            # from the sub-build
                            ${CMAKE_COMMAND} -E env --unset=DESTDIR ${CMAKE_COMMAND} --build . --target install
                          STEP_TARGETS
                            install
                          BUILD_BYPRODUCTS
                            <INSTALL_DIR>/${LIBDISPATCH_RUNTIME_DIR}/${SWIFT_SDK_${sdk}_STATIC_LIBRARY_PREFIX}dispatch${SWIFT_SDK_${sdk}_STATIC_LIBRARY_SUFFIX}
                            <INSTALL_DIR>/${LIBDISPATCH_RUNTIME_DIR}/${SWIFT_SDK_${sdk}_STATIC_LIBRARY_PREFIX}BlocksRuntime${SWIFT_SDK_${sdk}_STATIC_LIBRARY_SUFFIX}
                          BUILD_ALWAYS
                            1)

      ExternalProject_Get_Property("${LIBDISPATCH_STATIC_VARIANT_NAME}" install_dir)

      # CMake does not like the addition of INTERFACE_INCLUDE_DIRECTORIES without
      # the directory existing.  Just create the location which will be populated
      # during the installation.
      file(MAKE_DIRECTORY ${install_dir}/include)

      set(DISPATCH_STATIC_VARIANT_NAME "dispatch-${SWIFT_SDK_${sdk}_LIB_SUBDIR}-${arch}-static")
      add_library("${DISPATCH_STATIC_VARIANT_NAME}" STATIC IMPORTED GLOBAL)
      set_target_properties("${DISPATCH_STATIC_VARIANT_NAME}"
                            PROPERTIES
                              IMPORTED_LOCATION
                                ${install_dir}/${LIBDISPATCH_RUNTIME_DIR}/${SWIFT_SDK_${sdk}_STATIC_LIBRARY_PREFIX}dispatch${SWIFT_SDK_${sdk}_STATIC_LIBRARY_SUFFIX}
                              IMPORTED_IMPLIB
                                ${install_dir}/lib/${SWIFT_SDK_${sdk}_STATIC_LIBRARY_PREFIX}dispatch${SWIFT_SDK_${sdk}_STATIC_LIBRARY_SUFFIX}
                              INTERFACE_INCLUDE_DIRECTORIES
                                ${install_dir}/include)

      set(BLOCKS_RUNTIME_STATIC_VARIANT_NAME "BlocksRuntime-${SWIFT_SDK_${sdk}_LIB_SUBDIR}-${arch}-static")
      add_library("${BLOCKS_RUNTIME_STATIC_VARIANT_NAME}" STATIC IMPORTED GLOBAL)
      set_target_properties("${BLOCKS_RUNTIME_STATIC_VARIANT_NAME}"
                            PROPERTIES
                              IMPORTED_LOCATION
                                ${install_dir}/${LIBDISPATCH_RUNTIME_DIR}/${SWIFT_SDK_${sdk}_STATIC_LIBRARY_PREFIX}BlocksRuntime${SWIFT_SDK_${sdk}_STATIC_LIBRARY_SUFFIX}
                              IMPORTED_IMPLIB
                                ${install_dir}/lib/${SWIFT_SDK_${sdk}_STATIC_LIBRARY_PREFIX}BlocksRuntime${SWIFT_SDK_${sdk}_STATIC_LIBRARY_SUFFIX}
                              INTERFACE_INCLUDE_DIRECTORIES
                                ${SWIFT_PATH_TO_LIBDISPATCH_SOURCE}/src/BlocksRuntime)

      add_dependencies("${DISPATCH_STATIC_VARIANT_NAME}" "${LIBDISPATCH_STATIC_VARIANT_NAME}-install")
      add_dependencies("${BLOCKS_RUNTIME_STATIC_VARIANT_NAME}" "${LIBDISPATCH_STATIC_VARIANT_NAME}-install")
    endif()

    if("${sdk}" STREQUAL "${SWIFT_HOST_VARIANT_SDK}")
      if("${arch}" STREQUAL "${SWIFT_HOST_VARIANT_ARCH}")
        add_library(dispatch ALIAS "${DISPATCH_VARIANT_NAME}")
        add_library(BlocksRuntime ALIAS "${BLOCKS_RUNTIME_VARIANT_NAME}")
      endif()
    endif()
  endforeach()
endforeach()

if(DISPATCH_SDKS)
  # FIXME(compnerd) this should be taken care of by the
  # INTERFACE_INCLUDE_DIRECTORIES above
  include_directories(AFTER
                      ${SWIFT_PATH_TO_LIBDISPATCH_SOURCE}/src/BlocksRuntime
                      ${SWIFT_PATH_TO_LIBDISPATCH_SOURCE})
endif()
