//===--- Visibility.h - Visibility macros for runtime exports ---*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  These macros are used to declare symbols that should be exported from the
//  Swift runtime.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_STDLIB_SHIMS_VISIBILITY_H
#define SWIFT_STDLIB_SHIMS_VISIBILITY_H

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

#if !defined(__has_attribute)
#define __has_attribute(x) 0
#endif

#if !defined(__has_builtin)
#define __has_builtin(builtin) 0
#endif

#if !defined(__has_cpp_attribute)
#define __has_cpp_attribute(attribute) 0
#endif

// TODO: These macro definitions are duplicated in BridgedSwiftObject.h. Move
// them to a single file if we find a location that both Visibility.h and
// BridgedSwiftObject.h can import.
#if __has_feature(nullability)
// Provide macros to temporarily suppress warning about the use of
// _Nullable and _Nonnull.
# define SWIFT_BEGIN_NULLABILITY_ANNOTATIONS                        \
  _Pragma("clang diagnostic push")                                  \
  _Pragma("clang diagnostic ignored \"-Wnullability-extension\"")
# define SWIFT_END_NULLABILITY_ANNOTATIONS                          \
  _Pragma("clang diagnostic pop")

#else
// #define _Nullable and _Nonnull to nothing if we're not being built
// with a compiler that supports them.
# define _Nullable
# define _Nonnull
# define SWIFT_BEGIN_NULLABILITY_ANNOTATIONS
# define SWIFT_END_NULLABILITY_ANNOTATIONS
#endif

#define SWIFT_MACRO_CONCAT(A, B) A ## B
#define SWIFT_MACRO_IF_0(IF_TRUE, IF_FALSE) IF_FALSE
#define SWIFT_MACRO_IF_1(IF_TRUE, IF_FALSE) IF_TRUE
#define SWIFT_MACRO_IF(COND, IF_TRUE, IF_FALSE) \
  SWIFT_MACRO_CONCAT(SWIFT_MACRO_IF_, COND)(IF_TRUE, IF_FALSE)

#if __has_attribute(pure)
#define SWIFT_READONLY __attribute__((__pure__))
#else
#define SWIFT_READONLY
#endif

#if __has_attribute(const)
#define SWIFT_READNONE __attribute__((__const__))
#else
#define SWIFT_READNONE
#endif

#if __has_attribute(always_inline)
#define SWIFT_ALWAYS_INLINE __attribute__((always_inline))
#else
#define SWIFT_ALWAYS_INLINE
#endif

#if __has_attribute(noinline)
#define SWIFT_NOINLINE __attribute__((__noinline__))
#else
#define SWIFT_NOINLINE
#endif

#if __has_attribute(noreturn)
#define SWIFT_NORETURN __attribute__((__noreturn__))
#else
#define SWIFT_NORETURN
#endif

#if __has_attribute(used)
#define SWIFT_USED __attribute__((__used__))
#else
#define SWIFT_USED
#endif

#if __has_attribute(unavailable)
#define SWIFT_ATTRIBUTE_UNAVAILABLE __attribute__((__unavailable__))
#else
#define SWIFT_ATTRIBUTE_UNAVAILABLE
#endif

#if (__has_attribute(weak_import))
#define SWIFT_WEAK_IMPORT __attribute__((weak_import))
#else
#define SWIFT_WEAK_IMPORT
#endif

#if __has_attribute(musttail)
#define SWIFT_MUSTTAIL [[clang::musttail]]
#else
#define SWIFT_MUSTTAIL
#endif

// Define the appropriate attributes for sharing symbols across
// image (executable / shared-library) boundaries.
//
// SWIFT_ATTRIBUTE_FOR_EXPORTS will be placed on declarations that
// are known to be exported from the current image.  Typically, they
// are placed on header declarations and then inherited by the actual
// definitions.
//
// SWIFT_ATTRIBUTE_FOR_IMPORTS will be placed on declarations that
// are known to be exported from a different image.  This never
// includes a definition.
//
// Getting the right attribute on a declaration can be pretty awkward,
// but it's necessary under the C translation model.  All of this
// ceremony is familiar to Windows programmers; C/C++ programmers
// everywhere else usually don't bother, but since we have to get it
// right for Windows, we have everything set up to get it right on
// other targets as well, and doing so lets the compiler use more
// efficient symbol access patterns.
#if defined(__MACH__) || defined(__wasm__)

// On Mach-O and WebAssembly, we use non-hidden visibility.  We just use
// default visibility on both imports and exports, both because these
// targets don't support protected visibility but because they don't
// need it: symbols are not interposable outside the current image
// by default.
# define SWIFT_ATTRIBUTE_FOR_EXPORTS __attribute__((__visibility__("default")))
# define SWIFT_ATTRIBUTE_FOR_IMPORTS __attribute__((__visibility__("default")))

#elif defined(__ELF__)

// On ELF, we use non-hidden visibility.  For exports, we must use
// protected visibility to tell the compiler and linker that the symbols
// can't be interposed outside the current image.  For imports, we must
// use default visibility because protected visibility guarantees that
// the symbol is defined in the current library, which isn't true for
// an import.
//
// The compiler does assume that the runtime and standard library can
// refer to each other's symbols as DSO-local, so it's important that
// we get this right or we can get linker errors.
# define SWIFT_ATTRIBUTE_FOR_EXPORTS __attribute__((__visibility__("protected")))
# define SWIFT_ATTRIBUTE_FOR_IMPORTS __attribute__((__visibility__("default")))

#elif defined(__CYGWIN__)

// For now, we ignore all this on Cygwin.
# define SWIFT_ATTRIBUTE_FOR_EXPORTS
# define SWIFT_ATTRIBUTE_FOR_IMPORTS

// FIXME: this #else should be some sort of #elif Windows
#else // !__MACH__ && !__ELF__

// On PE/COFF, we use dllimport and dllexport.
# define SWIFT_ATTRIBUTE_FOR_EXPORTS __declspec(dllexport)
# define SWIFT_ATTRIBUTE_FOR_IMPORTS __declspec(dllimport)

#endif

// CMake conventionally passes -DlibraryName_EXPORTS when building
// code that goes into libraryName.  This isn't the best macro name,
// but it's conventional.  We do have to pass it explicitly in a few
// places in the build system for a variety of reasons.
//
// Unfortunately, defined(D) is a special function you can use in
// preprocessor conditions, not a macro you can use anywhere, so we
// need to manually check for all the libraries we know about so that
// we can use them in our condition below.s
#if defined(swiftCore_EXPORTS)
#define SWIFT_IMAGE_EXPORTS_swiftCore 1
#else
#define SWIFT_IMAGE_EXPORTS_swiftCore 0
#endif
#if defined(swift_Concurrency_EXPORTS)
#define SWIFT_IMAGE_EXPORTS_swift_Concurrency 1
#else
#define SWIFT_IMAGE_EXPORTS_swift_Concurrency 0
#endif
#if defined(swiftDistributed_EXPORTS)
#define SWIFT_IMAGE_EXPORTS_swiftDistributed 1
#else
#define SWIFT_IMAGE_EXPORTS_swiftDistributed 0
#endif
#if defined(swift_Differentiation_EXPORTS)
#define SWIFT_IMAGE_EXPORTS_swift_Differentiation 1
#else
#define SWIFT_IMAGE_EXPORTS_swift_Differentiation 0
#endif

#define SWIFT_EXPORT_FROM_ATTRIBUTE(LIBRARY)                          \
  SWIFT_MACRO_IF(SWIFT_IMAGE_EXPORTS_##LIBRARY,                       \
                 SWIFT_ATTRIBUTE_FOR_EXPORTS,                         \
                 SWIFT_ATTRIBUTE_FOR_IMPORTS)

// SWIFT_EXPORT_FROM(LIBRARY) declares something to be a C-linkage
// entity exported by the given library.
//
// SWIFT_RUNTIME_EXPORT is just SWIFT_EXPORT_FROM(swiftCore).
//
// TODO: use this in shims headers in overlays.
#if defined(__cplusplus)
#define SWIFT_EXPORT_FROM(LIBRARY) extern "C" SWIFT_EXPORT_FROM_ATTRIBUTE(LIBRARY)
#define SWIFT_EXTERN_C extern "C" 
#else
#define SWIFT_EXPORT_FROM(LIBRARY) SWIFT_EXPORT_FROM_ATTRIBUTE(LIBRARY)
#define SWIFT_EXTERN_C
#endif
#define SWIFT_RUNTIME_EXPORT SWIFT_EXPORT_FROM(swiftCore)
#define SWIFT_RUNTIME_EXPORT_ATTRIBUTE SWIFT_EXPORT_FROM_ATTRIBUTE(swiftCore)

#if __cplusplus > 201402l && __has_cpp_attribute(fallthrough)
#define SWIFT_FALLTHROUGH [[fallthrough]]
#elif __has_cpp_attribute(gnu::fallthrough)
#define SWIFT_FALLTHROUGH [[gnu::fallthrough]]
#elif __has_cpp_attribute(clang::fallthrough)
#define SWIFT_FALLTHROUGH [[clang::fallthrough]]
#elif __has_attribute(fallthrough)
#define SWIFT_FALLTHROUGH __attribute__((__fallthrough__))
#else
#define SWIFT_FALLTHROUGH
#endif

#if __cplusplus > 201402l && __has_cpp_attribute(nodiscard)
#define SWIFT_NODISCARD [[nodiscard]]
#elif __has_cpp_attribute(clang::warn_unused_result)
#define SWIFT_NODISCARD [[clang::warn_unused_result]]
#else
#define SWIFT_NODISCARD
#endif

#if __has_cpp_attribute(gnu::returns_nonnull)
#define SWIFT_RETURNS_NONNULL [[gnu::returns_nonnull]]
#elif defined(_MSC_VER) && defined(_Ret_notnull_)
#define SWIFT_RETURNS_NONNULL _Ret_notnull_
#else
#define SWIFT_RETURNS_NONNULL
#endif

/// Attributes for runtime-stdlib interfaces.
/// Use these for C implementations that are imported into Swift via SwiftShims
/// and for C implementations of Swift @_silgen_name declarations
/// Note that @_silgen_name implementations must also be marked SWIFT_CC(swift).
///
/// SWIFT_RUNTIME_STDLIB_API functions are called by compiler-generated code
/// or by @inlinable Swift code.
/// Such functions must be exported and must be supported forever as API.
/// The function name should be prefixed with `swift_`.
///
/// SWIFT_RUNTIME_STDLIB_SPI functions are called by overlay code.
/// Such functions must be exported, but are still SPI
/// and may be changed at any time.
/// The function name should be prefixed with `_swift_`.
///
/// SWIFT_RUNTIME_STDLIB_INTERNAL functions are called only by the stdlib.
/// Such functions are internal and are not exported.
#define SWIFT_RUNTIME_STDLIB_API       SWIFT_RUNTIME_EXPORT
#define SWIFT_RUNTIME_STDLIB_SPI       SWIFT_RUNTIME_EXPORT

// Match the definition of LLVM_LIBRARY_VISIBILITY from LLVM's
// Compiler.h. That header requires C++ and this needs to work in C.
#if __has_attribute(visibility) && (defined(__ELF__) || defined(__MACH__))
#define SWIFT_LIBRARY_VISIBILITY __attribute__ ((__visibility__("hidden")))
#else
#define SWIFT_LIBRARY_VISIBILITY
#endif

#if defined(__cplusplus)
#define SWIFT_RUNTIME_STDLIB_INTERNAL extern "C" SWIFT_LIBRARY_VISIBILITY
#else
#define SWIFT_RUNTIME_STDLIB_INTERNAL SWIFT_LIBRARY_VISIBILITY
#endif

#if __has_builtin(__builtin_expect)
#define SWIFT_LIKELY(expression) (__builtin_expect(!!(expression), 1))
#define SWIFT_UNLIKELY(expression) (__builtin_expect(!!(expression), 0))
#else
#define SWIFT_LIKELY(expression) ((expression))
#define SWIFT_UNLIKELY(expression) ((expression))
#endif

// SWIFT_STDLIB_SHIMS_VISIBILITY_H
#endif
