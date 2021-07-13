#===--- CMakeLists.txt - Build the core standard library -----------------===#
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
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
  ICU.swift
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
  Substring.swift
  SwiftNativeNSArray.swift
  ThreadLocalStorage.swift
  UIntBuffer.swift
  UnavailableStringAPIs.swift
  UnicodeEncoding.swift
  UnicodeHelpers.swift
  UnicodeParser.swift
  UnicodeScalarProperties.swift
  CharacterProperties.swift # ORDER DEPENDENCY: UnicodeScalarProperties.swift
  Unmanaged.swift
  UnmanagedOpaqueString.swift
  UnmanagedString.swift
  UnsafePointer.swift
  UnsafeRawPointer.swift
  UnsafeUninitializedPointer.swift
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
  FloatingPointRandom.swift
  Mirror.swift
  PlaygroundDisplay.swift
  CommandLine.swift
  SliceBuffer.swift
  SIMDVector.swift
  UnfoldSequence.swift
  VarArgs.swift
  Zip.swift
  "${SWIFT_SOURCE_DIR}/stdlib/linker-support/magic-symbols-for-install-name.c"
  )

set(SWIFTLIB_GYB_SOURCES
  ${SWIFTLIB_ESSENTIAL_GYB_SOURCES}
  SIMDConcreteOperations.swift.gyb
  SIMDVectorTypes.swift.gyb
  Tuple.swift.gyb
  )
set(GROUP_INFO_JSON_FILE ${CMAKE_CURRENT_SOURCE_DIR}/GroupInfo.json)
set(swift_core_link_flags "${SWIFT_RUNTIME_SWIFT_LINK_FLAGS}")
set(swift_core_framework_depends)
set(swift_core_private_link_libraries)
set(swift_stdlib_compile_flags "${SWIFT_RUNTIME_SWIFT_COMPILE_FLAGS}")
if(SWIFT_PRIMARY_VARIANT_SDK IN_LIST SWIFT_APPLE_PLATFORMS)
  list(APPEND swift_core_link_flags "-all_load")
  list(APPEND swift_core_private_link_libraries icucore)
else()
  # With the GNU linker the equivalent of -all_load is to tell the linker
  # --whole-archive before the archive and --no-whole-archive after (without
  # the second, it causes errors when the system libraries are told to
  # include everything). The best way to get it in there, according to the
  # documentation, is to put the flags in the target_link_libraries setting.

  # TODO: However, for the moment this actually makes things explode with an
  # incomplete runtime. This should be turned back on when more of the porting
  # effort has been completed.
  #set(LINK_FLAGS
  #  -Wl,--whole-archive swiftRuntime -Wl,--no-whole-archive)
  if("${SWIFT_PATH_TO_LIBICU_BUILD}" STREQUAL "")
    list(APPEND swift_core_private_link_libraries
      ${SWIFT_${SWIFT_PRIMARY_VARIANT_SDK}_${SWIFT_PRIMARY_VARIANT_ARCH}_ICU_UC}
      ${SWIFT_${SWIFT_PRIMARY_VARIANT_SDK}_${SWIFT_PRIMARY_VARIANT_ARCH}_ICU_I18N})
  else()
    list(APPEND swift_core_private_link_libraries -licui18nswift -licuucswift -licudataswift)
  endif()
endif()

if(SWIFT_PRIMARY_VARIANT_SDK STREQUAL CYGWIN)
  # TODO(compnerd) cache this variable to permit re-configuration
  execute_process(COMMAND "cygpath" "-u" "$ENV{SYSTEMROOT}" OUTPUT_VARIABLE ENV_SYSTEMROOT)
  list(APPEND swift_core_private_link_libraries "${ENV_SYSTEMROOT}/system32/psapi.dll")
elseif(SWIFT_PRIMARY_VARIANT_SDK STREQUAL FREEBSD)
  find_library(EXECINFO_LIBRARY execinfo)
  list(APPEND swift_core_private_link_libraries ${EXECINFO_LIBRARY})
elseif(SWIFT_PRIMARY_VARIANT_SDK STREQUAL LINUX)
  if(SWIFT_BUILD_STATIC_STDLIB)
    list(APPEND swift_core_private_link_libraries)
  endif()
elseif(SWIFT_PRIMARY_VARIANT_SDK STREQUAL WINDOWS)
  list(APPEND swift_core_private_link_libraries shell32;DbgHelp)
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

# STAGING: Temporarily avoids having to write #fileID in Swift.swiftinterface.
list(APPEND swift_stdlib_compile_flags "-Xfrontend" "-enable-experimental-concise-pound-file")
list(APPEND swift_stdlib_compile_flags "-Xfrontend" "-enable-ossa-modules")

if(SWIFT_CHECK_ESSENTIAL_STDLIB)
  add_swift_target_library(swift_stdlib_essential ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_STDLIB IS_STDLIB_CORE
      INSTALL_IN_COMPONENT never_install
      ${SWIFTLIB_ESSENTIAL}
      GYB_SOURCES
        ${SWIFTLIB_ESSENTIAL_GYB_SOURCES})
  target_link_libraries(swift_stdlib_essential ${RUNTIME_DEPENDENCY})
endif()

add_swift_target_library(swiftCore
                  ${SWIFT_STDLIB_LIBRARY_BUILD_TYPES} IS_STDLIB IS_STDLIB_CORE
                    ${SWIFTLIB_SOURCES}
                  GYB_SOURCES
                    ${SWIFTLIB_GYB_SOURCES}
                  # The copy_shim_headers target dependency is required to let the
                  # build system know that there's a rule to produce the shims
                  # directory, but is not sufficient to cause the object file to be rebuilt
                  # when the shim header changes.  Therefore, we pass both the target
                  # and the generated directory as dependencies.
                  FILE_DEPENDS
                    copy_shim_headers "${SWIFTLIB_DIR}/shims" ${GROUP_INFO_JSON_FILE}
                  SWIFT_COMPILE_FLAGS
                    ${swift_stdlib_compile_flags} -Xcc -DswiftCore_EXPORTS ${SWIFT_STANDARD_LIBRARY_SWIFT_FLAGS}
                  LINK_FLAGS
                    ${swift_core_link_flags}
                  PRIVATE_LINK_LIBRARIES
                    ${swift_core_private_link_libraries}
                  INCORPORATE_OBJECT_LIBRARIES
                    swiftRuntime swiftLLVMSupport swiftDemangling swiftStdlibStubs
                  FRAMEWORK_DEPENDS
                    ${swift_core_framework_depends}
                  INSTALL_IN_COMPONENT
                    stdlib)
