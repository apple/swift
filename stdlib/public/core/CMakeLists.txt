#===--- CMakeLists.txt - Build the core standard library -----------------===#
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
#===----------------------------------------------------------------------===#

# The list of sources without which it's impossible to build a core
# standard library.  Try to add new standard library sources to
# SWIFTLIB_SOURCES, below, rather than SWIFTLIB_ESSENTIAL, if
# possible, to improve layering.  Check that you got it right by
# configuring with -DSWIFT_CHECK_ESSENTIAL_STDLIB=YES
set(SWIFTLIB_ESSENTIAL
  ### PLEASE KEEP THIS LIST IN ALPHABETICAL ORDER ###
  # Some files can't be sorted alphabetically, see notes in the list below.
  Algorithm.swift
  ArrayBody.swift
  ArrayBuffer.swift
  ArrayBufferProtocol.swift
  ArrayCast.swift
  Array.swift
  ArrayShared.swift
  ArraySlice.swift
  ArrayType.swift
  ASCII.swift
  Assert.swift
  AssertCommon.swift
  BidirectionalCollection.swift
  Bitset.swift
  Bool.swift
  BridgeObjectiveC.swift
  BridgeStorage.swift
  BridgingBuffer.swift
  Builtin.swift
  BuiltinMath.swift
  Character.swift
  CocoaArray.swift
  Codable.swift
  Collection.swift
  CollectionAlgorithms.swift
  Comparable.swift
  CompilerProtocols.swift
  Sendable.swift
  ContiguousArray.swift
  ContiguouslyStored.swift
  ClosedRange.swift
  ContiguousArrayBuffer.swift
  CString.swift
  CTypes.swift
  DebuggerSupport.swift
  Dictionary.swift
  DictionaryBridging.swift
  DictionaryBuilder.swift
  DictionaryCasting.swift
  DictionaryStorage.swift
  DictionaryVariant.swift
  DropWhile.swift
  Dump.swift
  EmptyCollection.swift
  Equatable.swift
  ErrorType.swift
  ExistentialCollection.swift
  Filter.swift
  FixedArray.swift
  FlatMap.swift
  Flatten.swift
  FloatingPoint.swift
  Hashable.swift
  # WORKAROUND: This file name is not sorted alphabetically in the list because
  # if we do so, the compiler crashes.
  AnyHashable.swift
  # END WORKAROUND
  Hasher.swift
  Hashing.swift
  HashTable.swift
  Identifiable.swift
  Indices.swift
  InputStream.swift
  IntegerParsing.swift
  Integers.swift
  Join.swift
  KeyPath.swift
  KeyValuePairs.swift
  LazyCollection.swift
  LazySequence.swift
  LegacyABI.swift
  LifetimeManager.swift
  Macros.swift
  ManagedBuffer.swift
  Map.swift
  MemoryLayout.swift
  UnicodeScalar.swift # ORDER DEPENDENCY: Must precede Mirrors.swift
  Mirrors.swift
  Misc.swift
  MutableCollection.swift
  NativeDictionary.swift
  NativeSet.swift
  NewtypeWrapper.swift
  NFC.swift
  NFD.swift
  ObjectIdentifier.swift
  Optional.swift
  OptionSet.swift
  OutputStream.swift
  Pointer.swift
  Policy.swift
  PrefixWhile.swift
  Prespecialize.swift
  Print.swift
  PtrAuth.swift
  Random.swift
  RandomAccessCollection.swift
  Range.swift
  RangeReplaceableCollection.swift
  ReflectionMirror.swift
  Repeat.swift
  REPL.swift
  Result.swift
  Reverse.swift
  Runtime.swift
  RuntimeFunctionCounters.swift
  SipHash.swift
  Sequence.swift
  SequenceAlgorithms.swift
  Set.swift
  SetAlgebra.swift
  SetAnyHashableExtensions.swift
  SetBridging.swift
  SetBuilder.swift
  SetCasting.swift
  SetStorage.swift
  SetVariant.swift
  ShadowProtocols.swift
  Shims.swift
  Slice.swift
  SmallString.swift
  Sort.swift
  StaticString.swift
  StaticPrint.swift
  Stride.swift
  StringHashable.swift  # ORDER DEPENDENCY: Must precede String.swift
  String.swift
  StringBreadcrumbs.swift
  StringBridge.swift
  StringCharacterView.swift
  StringComparable.swift
  StringComparison.swift
  StringCreate.swift
  StringGuts.swift
  StringGutsSlice.swift
  StringGutsRangeReplaceable.swift
  StringObject.swift
  StringProtocol.swift
  StringIndex.swift
  StringIndexConversions.swift
  StringIndexValidation.swift
  StringInterpolation.swift
  StringLegacy.swift
  StringNormalization.swift
  StringRangeReplaceableCollection.swift
  StringStorage.swift
  StringStorageBridge.swift
  StringSwitch.swift
  StringTesting.swift
  StringUnicodeScalarView.swift
  StringUTF16View.swift
  StringUTF8View.swift
  StringUTF8Validation.swift
  StringWordBreaking.swift
  Substring.swift
  SwiftNativeNSArray.swift
  TemporaryAllocation.swift
  ThreadLocalStorage.swift
  UIntBuffer.swift
  UnavailableStringAPIs.swift
  UnicodeData.swift
  UnicodeEncoding.swift
  UnicodeBreakProperty.swift
  UnicodeHelpers.swift
  UnicodeParser.swift
  UnicodeScalarProperties.swift
  CharacterProperties.swift # ORDER DEPENDENCY: UnicodeScalarProperties.swift
  UnicodeSPI.swift
  Unmanaged.swift
  UnmanagedOpaqueString.swift
  UnmanagedString.swift
  UnsafePointer.swift
  UnsafeRawPointer.swift
  UTFEncoding.swift
  UTF8.swift
  UTF16.swift
  UTF32.swift
  Unicode.swift # ORDER DEPENDENCY: must follow new unicode support
  StringGraphemeBreaking.swift # ORDER DEPENDENCY: Must follow UTF16.swift
  ValidUTF8Buffer.swift
  WriteBackMutableSlice.swift
  MigrationSupport.swift)

set(SWIFTLIB_ESSENTIAL_GYB_SOURCES
  AtomicInt.swift.gyb
  FloatingPointParsing.swift.gyb
  FloatingPointTypes.swift.gyb
  IntegerTypes.swift.gyb
  UnsafeBufferPointer.swift.gyb
  UnsafeRawBufferPointer.swift.gyb
  )

# The complete list of sources in the core standard library.  Includes
# all the essential sources listed above.
set(SWIFTLIB_SOURCES
  ${SWIFTLIB_ESSENTIAL}
  ### PLEASE KEEP THIS LIST IN ALPHABETICAL ORDER ###
  Availability.swift
  CollectionDifference.swift
  CollectionOfOne.swift
  Diffing.swift
  Duration.swift
  DurationProtocol.swift
  FloatingPointRandom.swift
  Instant.swift
  Mirror.swift
  PlaygroundDisplay.swift
  CommandLine.swift
  SliceBuffer.swift
  StaticBigInt.swift
  UnfoldSequence.swift
  UnsafeBufferPointerSlice.swift
  VarArgs.swift
  Zip.swift
  "${SWIFT_SOURCE_DIR}/stdlib/linker-support/magic-symbols-for-install-name.c"
  )

set(SWIFTLIB_GYB_SOURCES
  ${SWIFTLIB_ESSENTIAL_GYB_SOURCES}
  Int128.swift.gyb
  Tuple.swift.gyb
  )

if(SWIFT_STDLIB_ENABLE_VECTOR_TYPES)
  list(APPEND SWIFTLIB_SOURCES SIMDVector.swift)
  list(APPEND SWIFTLIB_GYB_SOURCES SIMDConcreteOperations.swift.gyb SIMDVectorTypes.swift.gyb)
endif()

set(GROUP_INFO_JSON_FILE ${CMAKE_CURRENT_SOURCE_DIR}/GroupInfo.json)
set(swift_core_link_flags "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}")
set(swift_core_framework_depends)
set(swift_core_private_link_libraries)
set(swift_stdlib_compile_flags "${SWIFT_RUNTIME_SWIFT_COMPILE_FLAGS}")

if(SWIFT_PRIMARY_VARIANT_SDK STREQUAL "CYGWIN")
  # TODO(compnerd) cache this variable to permit re-configuration
  execute_process(COMMAND "cygpath" "-u" "$ENV{SYSTEMROOT}" OUTPUT_VARIABLE ENV_SYSTEMROOT)
  list(APPEND swift_core_private_link_libraries "${ENV_SYSTEMROOT}/system32/psapi.dll")
elseif(SWIFT_PRIMARY_VARIANT_SDK STREQUAL "FREEBSD")
  find_library(EXECINFO_LIBRARY execinfo)
  list(APPEND swift_core_private_link_libraries ${EXECINFO_LIBRARY})
elseif(SWIFT_PRIMARY_VARIANT_SDK STREQUAL "LINUX")
  if(SWIFT_BUILD_STATIC_STDLIB)
    list(APPEND swift_core_private_link_libraries)
  endif()
elseif(SWIFT_PRIMARY_VARIANT_SDK STREQUAL "WINDOWS")
  list(APPEND swift_core_private_link_libraries shell32;DbgHelp;Synchronization)
endif()

option(SWIFT_CHECK_ESSENTIAL_STDLIB
    "Check core standard library layering by linking its essential subset"
    FALSE)

if(NOT "${CMAKE_BUILD_TYPE}" STREQUAL "MinSizeRel")
  list(APPEND swift_stdlib_compile_flags "-Xllvm" "-sil-inline-generics")
  list(APPEND swift_stdlib_compile_flags "-Xllvm" "-sil-partial-specialization")
endif()
if(SWIFT_STDLIB_ENABLE_STDLIBCORE_EXCLUSIVITY_CHECKING)
  list(APPEND swift_stdlib_compile_flags "-enforce-exclusivity=checked")
endif()
if(SWIFT_STDLIB_ENABLE_DEBUG_PRECONDITIONS_IN_RELEASE)
  list(APPEND swift_stdlib_compile_flags "-DSWIFT_STDLIB_ENABLE_DEBUG_PRECONDITIONS_IN_RELEASE")
endif()
set(compile_flags_for_final_build)
if(SWIFT_ENABLE_ARRAY_COW_CHECKS)
  list(APPEND compile_flags_for_final_build "-DCOW_CHECKS_ENABLED")
endif()

# STAGING: Temporarily avoids having to write #fileID in Swift.swiftinterface.
list(APPEND swift_stdlib_compile_flags "-Xfrontend" "-enable-experimental-concise-pound-file")

list(APPEND swift_stdlib_compile_flags "-enable-experimental-feature" "Macros")
list(APPEND swift_stdlib_compile_flags "-enable-experimental-feature" "FreestandingMacros")

if(SWIFT_CHECK_ESSENTIAL_STDLIB)
  add_swift_target_library(swift_stdlib_essential ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_STDLIB IS_STDLIB_CORE
      INSTALL_IN_COMPONENT never_install
      ${SWIFTLIB_ESSENTIAL}
      GYB_SOURCES
        ${SWIFTLIB_ESSENTIAL_GYB_SOURCES})
  target_link_libraries(swift_stdlib_essential ${RUNTIME_DEPENDENCY})
endif()

set(swift_core_incorporate_object_libraries)
list(APPEND swift_core_incorporate_object_libraries swiftRuntime)
list(APPEND swift_core_incorporate_object_libraries swiftLLVMSupport)
list(APPEND swift_core_incorporate_object_libraries swiftDemangling)
list(APPEND swift_core_incorporate_object_libraries swiftStdlibStubs)
list(APPEND swift_core_incorporate_object_libraries swiftThreading)
if(SWIFT_STDLIB_HAS_COMMANDLINE)
  list(APPEND swift_core_incorporate_object_libraries swiftCommandLineSupport)
endif()

set(swiftCore_common_options
                  IS_STDLIB IS_STDLIB_CORE
                    ${SWIFTLIB_SOURCES}
                  GYB_SOURCES
                    ${SWIFTLIB_GYB_SOURCES}
                  LINK_FLAGS
                    ${swift_core_link_flags}
                  PRIVATE_LINK_LIBRARIES
                    ${swift_core_private_link_libraries}
                  INCORPORATE_OBJECT_LIBRARIES
                    ${swift_core_incorporate_object_libraries}
                  FRAMEWORK_DEPENDS
                    ${swift_core_framework_depends}
                  SWIFT_COMPILE_FLAGS
                    ${swift_stdlib_compile_flags} -Xcc -DswiftCore_EXPORTS ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS})

# The copy_shim_headers target dependency is required to let the
# build system know that there's a rule to produce the shims
# directory, but is not sufficient to cause the object file to be rebuilt
# when the shim header changes.  Therefore, we pass both the target
# and the generated directory as dependencies.
set(swiftCore_common_dependencies
    copy_shim_headers "${SWIFTLIB_DIR}/shims" ${GROUP_INFO_JSON_FILE})

if(BOOTSTRAPPING_MODE STREQUAL "BOOTSTRAPPING")

  set(b0_deps symlink-headers-bootstrapping0)
  set(b1_deps symlink-headers-bootstrapping1)

  list(FIND SWIFT_STDLIB_LIBRARY_BUILD_TYPES "SHARED" index_of_shared)
  if(index_of_shared EQUAL -1)
    message(FATAL_ERROR "bootstrapping requires SHARED stdlib build type")
  endif()

  set(swiftCore_common_bootstrapping_options
                    SHARED
                    ${swiftCore_common_options}
                    SDK ${SWIFT_HOST_VARIANT_SDK}
                    ARCHITECTURE ${SWIFT_HOST_VARIANT_ARCH}
                    INSTALL_IN_COMPONENT
                      "never_install")

  # Bootstrapping - stage 0

  add_swift_target_library_single(swiftCore-bootstrapping0 swiftCore
                    ${swiftCore_common_bootstrapping_options}
                    FILE_DEPENDS
                      ${b0_deps} ${swiftCore_common_dependencies}
                    BOOTSTRAPPING 0)

  # Bootstrapping - stage 1

  add_swift_target_library_single(swiftCore-bootstrapping1 swiftCore
                    ${swiftCore_common_bootstrapping_options}
                    FILE_DEPENDS
                      ${b1_deps} ${swiftCore_common_dependencies}
                    BOOTSTRAPPING 1)
endif()

add_swift_target_library(swiftCore
                  ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES}
                  ${swiftCore_common_options}
                  ${compile_flags_for_final_build}
                  FILE_DEPENDS
                    ${swiftCore_common_dependencies}
                  INSTALL_IN_COMPONENT
                    stdlib
                  MACCATALYST_BUILD_FLAVOR
                    zippered
                 )
