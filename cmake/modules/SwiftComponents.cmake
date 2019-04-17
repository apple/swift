# Swift Components
# ----------------
#
# This file contains the cmake code for initialization and manipulation of
# "Swift Components". A "Swift Component" is a disjoint set of source files,
# binary products, and source products that are inputs or products of Swift's
# cmake build system. At a high level each component can be viewed approximately
# as an individual package in a Debian-style Linux package (i.e. a .deb file).
#
# *NOTE* In the following for brevity, a "Swift Component" will just be called a
# component.
#
# For every cmake invocation, the set of components are split into the following
# sets: defined, include, build, and install components. These sets form a
# lattice as follows:
#
#    install => build => include => defined
#
# We describe the characteristics of each set below:
#
# 1. A "defined" component is a component that is known to the build system. It
# has defined source inputs and binary/source outputs. But the build system is
# not required to generate targets, run any targets associated with the package
# while building, or install any binary/source outputs associated with the
# component.
#
# 2. A "include" component is a "defined" component that cmake will generate
# targets for. This means that it will not be built or installed by default
# (i.e. it is not apart of the "all" target), but one can from the relevant
# cmake build command invoke the generated targets directly and any dependencies
# on the component from other packages will cause the package to be built and or
# installed.
#
# 3. A "build" component is a "include" component that cmake will add to the
# "all" target. This means that cmake will add this component to the "all"
# target causing it to be built by default when one invokes a build tool. On the
# other hand, the component is not guaranteed to be installed by default.
#
# 4. A "install" component is a "build" component that will have targets
# generated by default, will be built by default, and will be installed by
# default.
#
# Set of Defined Components
# -------------------------
#
# The set of "defined" swift components are as follows:
#
# * autolink-driver -- the Swift driver support tools
# * compiler -- the Swift compiler and (on supported platforms) the REPL.
# * clang-builtin-headers -- install a copy of Clang builtin headers under
#   'lib/swift/clang'.  This is useful when Swift compiler is installed in
#   isolation.
# * clang-resource-dir-symlink -- install a symlink to the Clang resource
#   directory (which contains builtin headers) under 'lib/swift/clang'.  This is
#   useful when Clang and Swift are installed side-by-side.
# * stdlib -- the Swift standard library.
# * stdlib-experimental -- the Swift standard library module for experimental
#   APIs.
# * sdk-overlay -- the Swift SDK overlay.
# * editor-integration -- scripts for Swift integration in IDEs other than
#   Xcode;
# * tools -- tools (other than the compiler) useful for developers writing
#   Swift code.
# * toolchain-tools -- a subset of tools that we will install to the OSS toolchain.
# * testsuite-tools -- extra tools required to run the Swift testsuite.
# * toolchain-dev-tools -- install development tools useful in a shared toolchain
# * dev -- headers and libraries required to use Swift compiler as a library.
set(_SWIFT_DEFINED_COMPONENTS
  "autolink-driver;compiler;clang-builtin-headers;clang-resource-dir-symlink;clang-builtin-headers-in-clang-resource-dir;stdlib;stdlib-experimental;sdk-overlay;parser-lib;editor-integration;tools;testsuite-tools;toolchain-tools;toolchain-dev-tools;dev;license;sourcekit-xpc-service;sourcekit-inproc;swift-remote-mirror;swift-remote-mirror-headers")

# The default install components include all of the defined components, except
# for the following exceptions.
set(_SWIFT_DEFAULT_COMPONENTS "${_SWIFT_DEFINED_COMPONENTS}")
# 'dev' takes up a lot of disk space and isn't part of a normal toolchain.
list(REMOVE_ITEM _SWIFT_DEFAULT_COMPONENTS "dev")
# These clang header options conflict with 'clang-builtin-headers'.
list(REMOVE_ITEM _SWIFT_DEFAULT_COMPONENTS "clang-resource-dir-symlink")
list(REMOVE_ITEM _SWIFT_DEFAULT_COMPONENTS "clang-builtin-headers-in-clang-resource-dir")
# This conflicts with LLVM itself when doing unified builds.
list(REMOVE_ITEM _SWIFT_DEFAULT_COMPONENTS "toolchain-dev-tools")
# The sourcekit install variants are currently mutually exclusive.
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  list(REMOVE_ITEM _SWIFT_DEFAULT_COMPONENTS "sourcekit-inproc")
else()
  list(REMOVE_ITEM _SWIFT_DEFAULT_COMPONENTS "sourcekit-xpc-service")
endif()
list(REMOVE_ITEM _SWIFT_DEFAULT_COMPONENTS "stdlib-experimental")

macro(swift_configure_components)
  # Set the SWIFT_INSTALL_COMPONENTS variable to the default value if it is not passed in via -D
  set(SWIFT_INSTALL_COMPONENTS "${_SWIFT_DEFAULT_COMPONENTS}" CACHE STRING
    "A semicolon-separated list of components to install from the set ${_SWIFT_DEFINED_COMPONENTS}")

  foreach(component ${_SWIFT_DEFINED_COMPONENTS})
    add_custom_target(${component})
    add_swift_install_target(${component} ${component})

    string(TOUPPER "${component}" var_name_piece)
    string(REPLACE "-" "_" var_name_piece "${var_name_piece}")
    set(SWIFT_INSTALL_${var_name_piece} FALSE)
  endforeach()

  add_custom_target(install-components)

  foreach(component ${SWIFT_INSTALL_COMPONENTS})
    if(NOT "${component}" IN_LIST _SWIFT_DEFINED_COMPONENTS)
      message(FATAL_ERROR "unknown install component: ${component}")
    endif()

    string(TOUPPER "${component}" var_name_piece)
    string(REPLACE "-" "_" var_name_piece "${var_name_piece}")
    if(NOT SWIFT_INSTALL_EXCLUDE_${var_name_piece})
      set(SWIFT_INSTALL_${var_name_piece} TRUE)
      add_dependencies(install-components install-${component})
    endif()
  endforeach()
endmacro()

# Sets the is_installing variable.
function(swift_is_installing_component component result_var_name)
  precondition(component MESSAGE "Component name is required")

  if("${component}" STREQUAL "never_install")
    set("${result_var_name}" FALSE PARENT_SCOPE)
  else()
    if(NOT "${component}" IN_LIST _SWIFT_DEFINED_COMPONENTS)
      message(FATAL_ERROR "unknown install component: ${component}")
    endif()

    string(TOUPPER "${component}" var_name_piece)
    string(REPLACE "-" "_" var_name_piece "${var_name_piece}")
    set("${result_var_name}" "${SWIFT_INSTALL_${var_name_piece}}" PARENT_SCOPE)
  endif()
endfunction()

function(swift_install_symlink_component component)
  cmake_parse_arguments(
      ARG # prefix
      "" # options
      "LINK_NAME;TARGET;DESTINATION" # single-value args
      "" # multi-value args
      ${ARGN})
  precondition(ARG_LINK_NAME MESSAGE "LINK_NAME is required")
  precondition(ARG_TARGET MESSAGE "TARGET is required")
  precondition(ARG_DESTINATION MESSAGE "DESTINATION is required")

  swift_is_installing_component("${component}" is_installing)
  if (NOT is_installing)
    return()
  endif()

  if(EXISTS ${LLVM_CMAKE_DIR}/LLVMInstallSymlink.cmake)
    set(INSTALL_SYMLINK ${LLVM_CMAKE_DIR}/LLVMInstallSymlink.cmake)
  endif()
  precondition(INSTALL_SYMLINK
    MESSAGE "LLVMInstallSymlink script must be available.")

  install(SCRIPT ${INSTALL_SYMLINK}
          CODE "install_symlink(${ARG_LINK_NAME} ${ARG_TARGET} ${ARG_DESTINATION})")
endfunction()
