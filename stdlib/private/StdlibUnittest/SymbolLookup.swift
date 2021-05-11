//===--- SymbolLookup.swift -----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#if canImport(Darwin)
  import Darwin
#elseif canImport(Glibc)
  import Glibc
#elseif os(WASI)
  import WASILibc
#elseif os(Windows)
  import CRT
  import WinSDK
#else
#error("Unsupported platform")
#endif

#if canImport(Darwin) || os(OpenBSD)
  let RTLD_DEFAULT = UnsafeMutableRawPointer(bitPattern: -2)
#elseif os(Linux) || os(WASI)
  let RTLD_DEFAULT = UnsafeMutableRawPointer(bitPattern: 0)
#elseif os(Android)
  #if arch(arm) || arch(i386)
    let RTLD_DEFAULT = UnsafeMutableRawPointer(bitPattern: 0xffffffff as UInt)
  #elseif arch(arm64) || arch(x86_64)
    let RTLD_DEFAULT = UnsafeMutableRawPointer(bitPattern: 0)
  #else
    #error("Unsupported platform")
  #endif
#elseif os(Windows)
  let hStdlibCore: HMODULE = GetModuleHandleA("swiftCore.dll")!
#elseif os(WASI)
// WASI doesn't support dynamic linking yet.
#else
  #error("Unsupported platform")
#endif

public func pointerToSwiftCoreSymbol(name: String) -> UnsafeMutableRawPointer? {
#if os(Windows)
  return unsafeBitCast(GetProcAddress(hStdlibCore, name),
                       to: UnsafeMutableRawPointer?.self)
#elseif os(WASI)
  fatalError("\(#function) is not supported on WebAssembly/WASI")
#else
  return dlsym(RTLD_DEFAULT, name)
#endif
}
