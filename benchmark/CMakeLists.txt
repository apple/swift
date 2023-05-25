# -*- mode: cmake -*-

cmake_minimum_required(VERSION 3.19.6)

# Add path for custom CMake modules.
list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

# Load utility modules.
include(SwiftBenchmarkUtils)

set(SWIFT_BENCHMARK_BUILT_STANDALONE FALSE)
if("${CMAKE_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}")
  set(SWIFT_BENCHMARK_BUILT_STANDALONE TRUE)
endif()

if(SWIFT_BENCHMARK_SUBCMAKE_BUILD)
  precondition(SWIFT_BENCHMARK_BUILT_STANDALONE
    MESSAGE "If we are a subcmake build, we must be built standalone")
endif()

include(AddSwiftBenchmarkSuite)

#===-----------------------------------------------------------------------===#
# Declarative Description of Benchmarks
#===-----------------------------------------------------------------------===#

set(SWIFT_BENCH_MODULES
    single-source/Ackermann
    single-source/AngryPhonebook
    single-source/AnyHashableWithAClass
    single-source/Array2D
    single-source/ArrayAppend
    single-source/ArrayInClass
    single-source/ArrayLiteral
    single-source/ArrayOfGenericPOD
    single-source/ArrayOfGenericRef
    single-source/ArrayOfPOD
    single-source/ArrayOfRef
    single-source/ArrayRemoveAll
    single-source/ArraySetElement
    single-source/ArraySubscript
    single-source/BinaryFloatingPointConversionFromBinaryInteger
    single-source/BinaryFloatingPointProperties
    single-source/BitCount
    single-source/Breadcrumbs
    single-source/BucketSort
    single-source/BufferFill
    single-source/BufferFind
    single-source/ByteSwap
    single-source/COWTree
    single-source/COWArrayGuaranteedParameterOverhead
    single-source/CString
    single-source/CSVParsing
    single-source/Calculator
    single-source/CaptureProp
    single-source/ChaCha
    single-source/ChainedFilterMap
    single-source/CharacterLiteralsLarge
    single-source/CharacterLiteralsSmall
    single-source/CharacterProperties
    single-source/CharacterRecognizer
    single-source/Chars
    single-source/ClassArrayGetter
    single-source/CodableTest
    single-source/Combos
    single-source/DataBenchmarks
    single-source/DeadArray
    single-source/DevirtualizeProtocolComposition
    single-source/DictOfArraysToArrayOfDicts
    single-source/DictTest
    single-source/DictTest2
    single-source/DictTest3
    single-source/DictTest4
    single-source/DictTest4Legacy
    single-source/DictionaryBridge
    single-source/DictionaryBridgeToObjC
    single-source/DictionaryCompactMapValues
    single-source/DictionaryCopy
    single-source/DictionaryGroup
    single-source/DictionaryKeysContains
    single-source/DictionaryLiteralTest
    single-source/DictionaryOfAnyHashableStrings
    single-source/DictionaryRemove
    single-source/DictionarySubscriptDefault
    single-source/DictionarySwap
    single-source/Differentiation
    single-source/Diffing
    single-source/DiffingMyers
    single-source/DropFirst
    single-source/DropLast
    single-source/DropWhile
    single-source/ErrorHandling
    single-source/Exclusivity
    single-source/ExistentialPerformance
    single-source/Fibonacci
    single-source/FindStringNaive
    single-source/FlattenList
    single-source/FloatingPointConversion
    single-source/FloatingPointParsing
    single-source/FloatingPointPrinting
    single-source/Hanoi
    single-source/Hash
    single-source/Histogram
    single-source/HTTP2StateMachine
    single-source/IndexPathTest
    single-source/InsertCharacter
    single-source/IntegerParsing
    single-source/Integrate
    single-source/IterateData
    single-source/Join
    single-source/KeyPathPerformanceTests
    single-source/LazyFilter
    single-source/LinkedList
    single-source/LuhnAlgoEager
    single-source/LuhnAlgoLazy
    single-source/MapReduce
    single-source/Memset
    single-source/MirrorTest
    single-source/MonteCarloE
    single-source/MonteCarloPi
    single-source/NSDictionaryCastToSwift
    single-source/NSErrorTest
    single-source/NSStringConversion
    single-source/NaiveRangeReplaceableCollectionConformance
    single-source/NibbleSort
    single-source/NIOChannelPipeline
    single-source/NopDeinit
    single-source/ObjectAllocation
    single-source/ObjectiveCBridging
    single-source/ObjectiveCBridgingStubs
    single-source/ObjectiveCNoBridgingStubs
    single-source/ObserverClosure
    single-source/ObserverForwarderStruct
    single-source/ObserverPartiallyAppliedMethod
    single-source/ObserverUnappliedMethod
    single-source/OpaqueConsumingUsers
    single-source/OpenClose
    single-source/Phonebook
    single-source/PointerArithmetics
    single-source/PolymorphicCalls
    single-source/PopFront
    single-source/PopFrontGeneric
    single-source/Prefix
    single-source/PrefixWhile
    single-source/Prims
    single-source/PrimsNonStrongRef
    single-source/ProtocolConformance
    single-source/ProtocolDispatch
    single-source/ProtocolDispatch2
    single-source/QueueTest
    single-source/RC4
    single-source/RGBHistogram
    single-source/Radix2CooleyTukey
    single-source/RandomShuffle
    single-source/RandomTree
    single-source/RandomValues
    single-source/RangeAssignment
    single-source/RangeIteration
    single-source/RangeOverlaps
    single-source/RangeReplaceableCollectionPlusDefault
    single-source/RecursiveOwnedParameter
    single-source/ReduceInto
    single-source/RemoveWhere
    single-source/ReversedCollections
    single-source/RomanNumbers
    single-source/SIMDRandomMask
    single-source/SIMDReduceInteger
    # Disabled while layout prespecializations are experimental
    #single-source/SimpleArraySpecialization
    single-source/SequenceAlgos
    single-source/SetTests
    single-source/SevenBoom
    single-source/Sim2DArray
    single-source/SortArrayInClass
    single-source/SortIntPyramids
    single-source/SortLargeExistentials
    single-source/SortLettersInPlace
    single-source/SortStrings
    single-source/StackPromo
    single-source/StaticArray
    single-source/StrComplexWalk
    single-source/StrToInt
    single-source/StringBuilder
    single-source/StringComparison
    single-source/StringDistance
    single-source/StringEdits
    single-source/StringEnum
    single-source/StringInterpolation
    single-source/StringMatch
    single-source/StringRemoveDupes
    single-source/StringRepeating
    single-source/StringReplaceSubrange
    single-source/StringSplitting
    single-source/StringSwitch
    single-source/StringTests
    single-source/StringWalk
    single-source/SubstringTest
    single-source/Suffix
    single-source/SuperChars
    single-source/TwoSum
    single-source/TypeFlood
    single-source/UTF8Decode
    single-source/UTF16Decode
    single-source/Walsh
    single-source/WordCount
    single-source/XorLoop
    cxx-source/CreateObjects
    cxx-source/CxxSetToCollection
    cxx-source/CxxStringConversion
    cxx-source/CxxVectorSum
    cxx-source/ReadAccessor
)

set(SWIFT_MULTISOURCE_SWIFT_BENCHES
  multi-source/PrimsSplit
)

set(PrimsSplit_sources
    multi-source/PrimsSplit/Prims.swift
    multi-source/PrimsSplit/Prims_main.swift)

set(BENCH_DRIVER_LIBRARY_FLAGS)
if (SWIFT_RUNTIME_ENABLE_LEAK_CHECKER)
  set(BENCH_DRIVER_LIBRARY_FLAGS -DSWIFT_RUNTIME_ENABLE_LEAK_CHECKER)
endif()

set(BENCH_LIBRARY_MODULES
    utils/TestsUtils
    # Disabled while layout prespecializations are experimental
    #utils/SimpleArray
)

set(BENCH_DRIVER_LIBRARY_MODULES
    utils/DriverUtils
)

#===-----------------------------------------------------------------------===#
# Build Configuration
#===-----------------------------------------------------------------------===#

# You have to delete CMakeCache.txt in the swift build to force a
# reconfiguration.
set(SWIFT_EXTRA_BENCH_CONFIGS CACHE STRING
    "A semicolon separated list of benchmark configurations. \
Available configurations: <Optlevel>_SINGLEFILE, <Optlevel>_MULTITHREADED")

# Syntax for an optset:  <optimization-level>_<configuration>
#    where "_<configuration>" is optional.
if(NOT SWIFT_OPTIMIZATION_LEVELS)
  set(SWIFT_OPTIMIZATION_LEVELS "Onone" "O" "Osize"
                                ${SWIFT_EXTRA_BENCH_CONFIGS})
endif()

set(SWIFT_BENCHMARK_EXTRA_FLAGS "" CACHE STRING
    "Extra options to pass to swiftc when building the benchmarks")

set(SWIFT_BENCHMARK_UNOPTIMIZED_DRIVER NO CACHE BOOL
    "Build the benchmark driver utilities without optimization (default: no)")

if (SWIFT_BENCHMARK_BUILT_STANDALONE)
  # This option's value must match the value of the same option used when
  # building the swift runtime.
  option(SWIFT_RUNTIME_ENABLE_LEAK_CHECKER
    "Should the runtime be built with support for non-thread-safe leak detecting entrypoints"
    FALSE)
endif()

set(SWIFT_BENCHMARK_NUM_O_ITERATIONS "" CACHE STRING
    "Number of iterations to perform when running -O benchmarks via cmake")
set(SWIFT_BENCHMARK_NUM_ONONE_ITERATIONS "" CACHE STRING
    "Number of iterations to perform when running -Onone benchmarks via cmake")

# Options for the default (= empty) configuration
set(BENCHOPTS "-whole-module-optimization")

option(SWIFT_BENCHMARK_GENERATE_OPT_VIEW
  "Produce optimization view for benchmarks"
  FALSE)

set(SWIFT_BENCHMARK_OPT_VIEWER "" CACHE FILEPATH
  "Path to opt-viewer")

option(SWIFT_BENCHMARK_GENERATE_DEBUG_INFO
  "Produce debug info for benchmarks"
  TRUE)

if(SWIFT_BENCHMARK_OPT_VIEWER)
  # If the path to the opt-viewer was specified manually and we have no access
  # to the LLVM tree, assume we have the modules for the opt-viewer installed.
  if(SWIFT_BENCHMARK_BUILT_STANDALONE)
    set(LLVM_HAVE_OPT_VIEWER_MODULES 1)
  endif()
else()
  set (SWIFT_BENCHMARK_OPT_VIEWER
    "${LLVM_BUILD_MAIN_SRC_DIR}/tools/opt-viewer/opt-viewer.py")
endif()

# Options for other configurations
set(BENCHOPTS_MULTITHREADED
    "-whole-module-optimization" "-num-threads" "4")
set(BENCHOPTS_SINGLEFILE "")

option(SWIFT_BENCHMARK_USE_OS_LIBRARIES
  "Runtime link against the Swift libraries on the target (/usr/lib/swift)."
  FALSE)

configure_build()

#===-----------------------------------------------------------------------===#
# SDK Configuration
#===-----------------------------------------------------------------------===#

configure_sdks()

#===---------------------------------------------------------------------===#
# Statement of Configuration for Build Users
#===---------------------------------------------------------------------===#

message("--")
message("-- Swift Benchmark Suite:")
message("--   SWIFT_BENCHMARK_BUILT_STANDALONE = ${SWIFT_BENCHMARK_BUILT_STANDALONE}")
message("--   SWIFT_BENCHMARK_USE_OS_LIBRARIES = ${SWIFT_BENCHMARK_USE_OS_LIBRARIES}")
message("--   SWIFT_EXEC         = ${SWIFT_EXEC}")
message("--   SWIFT_LIBRARY_PATH = ${SWIFT_LIBRARY_PATH}")
if (SWIFT_RPATH_BASE)
message("--   SWIFT_RPATH_BASE   = ${SWIFT_RPATH_BASE}")
endif()
if (SWIFT_RPATH)
message("--   SWIFT_RPATH        = ${SWIFT_RPATH}")
message("---  ** WARNING ** Benchmarking against Swift-in-the-OS")
endif()
message("--   CLANG_EXEC         = ${CLANG_EXEC}")
message("--   SWIFT_BENCHMARK_EXTRA_FLAGS = ${SWIFT_BENCHMARK_EXTRA_FLAGS}")
message("--   SWIFT_OPTIMIZATION_LEVELS = ${SWIFT_OPTIMIZATION_LEVELS}")
message("--   ONLY_PLATFORMS = ${ONLY_PLATFORMS}")
message("--   PAGE_ALIGNMENT_OPTION = ${PAGE_ALIGNMENT_OPTION}")

message("--   found platforms: ${platforms}")
message("--   found sdks:")
foreach(sdk ${sdks})
  message("--     ${sdk}")
endforeach()

#===---------------------------------------------------------------------===#
# Build Rule Generation
#===---------------------------------------------------------------------===#

set(executable_targets)

set(srcdir "${CMAKE_CURRENT_SOURCE_DIR}")

if(SWIFT_BENCHMARK_BUILT_STANDALONE)
  set(swift-bin-dir "${CMAKE_BINARY_DIR}/bin")
else()
  get_filename_component(swift-bin-dir "${SWIFT_EXEC}" DIRECTORY)
endif()

set(benchmark-bin-dir "${CMAKE_CURRENT_BINARY_DIR}/bin")
set(benchmark-lib-dir "${CMAKE_CURRENT_BINARY_DIR}/lib")
set(benchmark-lib-swift-dir "${CMAKE_CURRENT_BINARY_DIR}/lib/swift")

file(MAKE_DIRECTORY "${swift-bin-dir}")
file(MAKE_DIRECTORY "${benchmark-bin-dir}")
file(MAKE_DIRECTORY "${benchmark-lib-dir}")
file(MAKE_DIRECTORY "${benchmark-lib-swift-dir}")

# Compile the perf test suite for each platform
foreach(platform ${platforms})
  swift_benchmark_compile(PLATFORM ${platform})
endforeach()

add_subdirectory(scripts)
