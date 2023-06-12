//===--- Context.swift - Unwind context structure -------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Defines the Context protocol and some concrete implementations for various
// different types of CPU.
//
// Context holds register values during unwinding.
//
//===----------------------------------------------------------------------===//

import Swift

#if os(macOS) || os(iOS) || os(watchOS) || os(tvOS)
@_implementationOnly import OS.Darwin
#elseif os(Linux)
@_implementationOnly import OS.Linux
#endif

@_implementationOnly import FixedLayout

@_spi(Contexts) public enum ContextError: Error {
  case unableToFormTLSAddress
}

@_spi(Contexts) public protocol Context: CustomStringConvertible {
  /// Represents a machine address for this type of machine
  associatedtype Address: FixedWidthInteger

  /// Represents a size for this type of machine
  associatedtype Size: FixedWidthInteger

  /// The type of a general purpose register on this machine
  associatedtype GPRValue: FixedWidthInteger

  /// An enumerated type defining the registers for the machine (this comes
  /// from the architecture specific DWARF specification).
  associatedtype Register: RawRepresentable where Register.RawValue == Int

  /// The architecture tag for this context (e.g. arm64, x86_64)
  var architecture: String { get }

  /// The program counter; this is likely a return address
  var programCounter: GPRValue { get set }

  /// The stack pointer
  var stackPointer: GPRValue { get set }

  /// The frame pointer
  var framePointer: GPRValue { get set }

  /// The CFA as defined by the relevant architecture specific DWARF
  /// specification.  For the architectures we have currently, it turns out
  /// that this is the stack pointer, but it might in general be some other
  /// thing.
  var callFrameAddress: GPRValue { get set }

  /// The number of register slots to reserve in the unwinder (this corresponds
  /// to the DWARF register numbers, which is why some of these reserve a lot
  /// of slots).
  static var registerCount: Int { get }

  /// Given a thread local address, form a genuine machine address
  func formTLSAddress(threadLocal: Address) throws -> Address

  /// Get the value of the specified general purpose register, or nil if unknown
  func getRegister(_ register: Register) -> GPRValue?

  /// Set the value of the specified general purpose register (or mark it as
  /// unknown if nil is passed)
  mutating func setRegister(_ register: Register, to value: GPRValue?)

  /// Set all of the registers in bulk
  mutating func setRegisters(_ registers: [GPRValue?])

  /// Strip any pointer authentication that might apply from an address.
  static func stripPtrAuth(address: Address) -> Address

  /// Test if an address is appropriately aligned for the stack.
  static func isAlignedForStack(framePointer: Address) -> Bool
}

extension Context {
  public func formTLSAddress(threadLocal: Address) throws -> Address {
    throw ContextError.unableToFormTLSAddress
  }

  public mutating func setRegisters(_ registers: [GPRValue?]) {
    for (ndx, value) in registers.enumerated() {
      if let reg = Register(rawValue: ndx) {
        setRegister(reg, to: value)
      }
    }
  }

  public static func stripPtrAuth(address: Address) -> Address {
    return address
  }
}

// .. Extensions to the GPR structures .........................................

// We need these because the arrays in the _gprs structs (which are defined
// in C so that the layout is fixed) get imported as tuples.

extension x86_64_gprs {
  func getR(_ ndx: Int) -> UInt64 {
    return withUnsafePointer(to: _r) {
      $0.withMemoryRebound(to: UInt64.self, capacity: 16) {
        $0[ndx]
      }
    }
  }

  mutating func setR(_ ndx: Int, to value: UInt64) {
    withUnsafeMutablePointer(to: &_r) {
      $0.withMemoryRebound(to: UInt64.self, capacity: 16) {
        $0[ndx] = value
      }
    }
    valid |= 1 << ndx
  }
}

extension i386_gprs {
  func getR(_ ndx: Int) -> UInt32 {
    return withUnsafePointer(to: _r) {
      $0.withMemoryRebound(to: UInt32.self, capacity: 8) {
        $0[ndx]
      }
    }
  }

  mutating func setR(_ ndx: Int, to value: UInt32) {
    withUnsafeMutablePointer(to: &_r) {
      $0.withMemoryRebound(to: UInt32.self, capacity: 8) {
        $0[ndx] = value
      }
    }
    valid |= 1 << ndx
  }
}

extension arm64_gprs {
  func getX(_ ndx: Int) -> UInt64 {
    return withUnsafePointer(to: _x) {
      $0.withMemoryRebound(to: UInt64.self, capacity: 32) {
        $0[ndx]
      }
    }
  }

  mutating func setX(_ ndx: Int, to value: UInt64) {
    withUnsafeMutablePointer(to: &_x) {
      $0.withMemoryRebound(to: UInt64.self, capacity: 32) {
        $0[ndx] = value
      }
    }
    valid |= 1 << ndx
  }
}

extension arm_gprs {
  func getR(_ ndx: Int) -> UInt32 {
    return withUnsafePointer(to: _r) {
      $0.withMemoryRebound(to: UInt32.self, capacity: 16) {
        $0[ndx]
      }
    }
  }

  mutating func setR(_ ndx: Int, to value: UInt32) {
    withUnsafeMutablePointer(to: &_r) {
      $0.withMemoryRebound(to: UInt32.self, capacity: 16) {
        $0[ndx] = value
      }
    }
    valid |= 1 << ndx
  }
}

// .. x86-64 ...................................................................

@_spi(Contexts) public struct X86_64Context: Context {
  public typealias Address = UInt64
  public typealias Size = UInt64
  public typealias GPRValue = UInt64
  public typealias Register = X86_64Register

  var gprs = x86_64_gprs()

  public var architecture: String { "x86_64" }

  public var programCounter: Address {
    get { return gprs.rip }
    set {
      gprs.rip = newValue
      gprs.valid |= 1 << 20
    }
  }
  public var framePointer: Address {
    get { return gprs.getR(X86_64Register.rbp.rawValue) }
    set {
      gprs.setR(X86_64Register.rbp.rawValue, to: newValue)
    }
  }
  public var stackPointer: Address {
    get { return gprs.getR(X86_64Register.rsp.rawValue) }
    set {
      gprs.setR(X86_64Register.rsp.rawValue, to: newValue)
    }
  }

  public var callFrameAddress: GPRValue {
    get { return stackPointer }
    set { stackPointer = newValue }
  }

  public static var registerCount: Int { return 56 }

  #if os(macOS) && arch(x86_64)
  init?(from thread: thread_t) {
    var state = darwin_x86_64_thread_state()
    let kr = thread_get_state(thread,
                              X86_THREAD_STATE64,
                              &state)
    if kr != KERN_SUCCESS {
      return nil
    }

    self.init(from: state)
  }

  init(with mctx: darwin_x86_64_mcontext) {
    self.init(from: mctx.ss)
  }

  init(from state: darwin_x86_64_thread_state) {
    gprs.setR(X86_64Register.rax.rawValue, to: state.rax)
    gprs.setR(X86_64Register.rbx.rawValue, to: state.rbx)
    gprs.setR(X86_64Register.rcx.rawValue, to: state.rcx)
    gprs.setR(X86_64Register.rdx.rawValue, to: state.rdx)
    gprs.setR(X86_64Register.rdi.rawValue, to: state.rdi)
    gprs.setR(X86_64Register.rsi.rawValue, to: state.rsi)
    gprs.setR(X86_64Register.rbp.rawValue, to: state.rbp)
    gprs.setR(X86_64Register.rsp.rawValue, to: state.rsp)
    gprs.setR(X86_64Register.r8.rawValue, to: state.r8)
    gprs.setR(X86_64Register.r9.rawValue, to: state.r9)
    gprs.setR(X86_64Register.r10.rawValue, to: state.r10)
    gprs.setR(X86_64Register.r11.rawValue, to: state.r11)
    gprs.setR(X86_64Register.r12.rawValue, to: state.r12)
    gprs.setR(X86_64Register.r13.rawValue, to: state.r13)
    gprs.setR(X86_64Register.r14.rawValue, to: state.r14)
    gprs.setR(X86_64Register.r15.rawValue, to: state.r15)
    gprs.rip = state.rip
    gprs.rflags = state.rflags
    gprs.cs = UInt16(state.cs)
    gprs.fs = UInt16(state.fs)
    gprs.gs = UInt16(state.gs)
    gprs.valid = 0x1fffff
  }

  public static func fromHostThread(_ thread: Any) -> HostContext? {
    return X86_64Context(from: thread as! thread_t)
  }

  public static func fromHostMContext(_ mcontext: Any) -> HostContext {
    return X86_64Context(with: mcontext as! darwin_x86_64_mcontext)
  }
  #elseif os(Linux) && arch(x86_64)
  init(with mctx: mcontext_t) {
    gprs.setR(X86_64Register.rax.rawValue, to: UInt64(bitPattern: mctx.gregs.13))
    gprs.setR(X86_64Register.rbx.rawValue, to: UInt64(bitPattern: mctx.gregs.12))
    gprs.setR(X86_64Register.rcx.rawValue, to: UInt64(bitPattern: mctx.gregs.14))
    gprs.setR(X86_64Register.rdx.rawValue, to: UInt64(bitPattern: mctx.gregs.11))
    gprs.setR(X86_64Register.rdi.rawValue, to: UInt64(bitPattern: mctx.gregs.9))
    gprs.setR(X86_64Register.rsi.rawValue, to: UInt64(bitPattern: mctx.gregs.8))
    gprs.setR(X86_64Register.rbp.rawValue, to: UInt64(bitPattern: mctx.gregs.10))
    gprs.setR(X86_64Register.rsp.rawValue, to: UInt64(bitPattern: mctx.gregs.15))
    gprs.setR(X86_64Register.r8.rawValue, to: UInt64(bitPattern: mctx.gregs.0))
    gprs.setR(X86_64Register.r9.rawValue, to: UInt64(bitPattern: mctx.gregs.1))
    gprs.setR(X86_64Register.r10.rawValue, to: UInt64(bitPattern: mctx.gregs.2))
    gprs.setR(X86_64Register.r11.rawValue, to: UInt64(bitPattern: mctx.gregs.3))
    gprs.setR(X86_64Register.r12.rawValue, to: UInt64(bitPattern: mctx.gregs.4))
    gprs.setR(X86_64Register.r13.rawValue, to: UInt64(bitPattern: mctx.gregs.5))
    gprs.setR(X86_64Register.r14.rawValue, to: UInt64(bitPattern: mctx.gregs.6))
    gprs.setR(X86_64Register.r15.rawValue, to: UInt64(bitPattern: mctx.gregs.7))
    gprs.rip = UInt64(bitPattern: mctx.gregs.16)
    gprs.rflags = UInt64(bitPattern: mctx.gregs.17)
    gprs.cs = UInt16(mctx.gregs.18 & 0xffff)
    gprs.fs = UInt16((mctx.gregs.18 >> 16) & 0xffff)
    gprs.gs = UInt16((mctx.gregs.18 >> 32) & 0xffff)
    gprs.valid = 0x1fffff
  }

  public static func fromHostMContext(_ mcontext: Any) -> HostContext {
    return X86_64Context(with: mcontext as! mcontext_t)
  }
  #endif

  #if os(Windows) || !SWIFT_ASM_AVAILABLE
  struct NotImplemented: Error {}
  public static func withCurrentContext<T>(fn: (X86_64Context) throws -> T) throws -> T {
    throw NotImplemented()
  }
  #elseif arch(x86_64)
  @usableFromInline
  @_silgen_name("_swift_get_cpu_context")
  static func _swift_get_cpu_context() -> X86_64Context

  @_transparent
  public static func withCurrentContext<T>(fn: (X86_64Context) throws -> T) rethrows -> T {
    return try fn(_swift_get_cpu_context())
  }
  #endif

  private func validNdx(_ register: Register) -> Int? {
    switch register {
      case .rax ... .r15:
        return register.rawValue
      case .rflags:
        return 16
      case .cs:
        return 17
      case .fs:
        return 18
      case .gs:
        return 19
      default:
        return nil
    }
  }

  private func isValid(_ register: Register) -> Bool {
    guard let ndx = validNdx(register) else {
      return false
    }
    return (gprs.valid & (UInt64(1) << ndx)) != 0
  }

  private mutating func setValid(_ register: Register) {
    guard let ndx = validNdx(register) else {
      return
    }
    gprs.valid |= UInt64(1) << ndx
  }

  private mutating func clearValid(_ register: Register) {
    guard let ndx = validNdx(register) else {
      return
    }
    gprs.valid &= ~(UInt64(1) << ndx)
  }

  public func getRegister(_ register: Register) -> GPRValue? {
    if !isValid(register) {
      return nil
    }

    switch register {
      case .rax ... .r15:
        return gprs.getR(register.rawValue)
      case .rflags: return gprs.rflags
      case .cs: return UInt64(gprs.cs)
      case .fs: return UInt64(gprs.fs)
      case .gs: return UInt64(gprs.gs)
      default:
        return nil
    }
  }

  public mutating func setRegister(_ register: Register, to value: GPRValue?) {
    if let value = value {
      switch register {
        case .rax ... .r15:
          gprs.setR(register.rawValue, to: value)
        case .rflags:
          gprs.rflags = value
          setValid(register)
        case .cs:
          gprs.cs = UInt16(value)
          setValid(register)
        case .fs:
          gprs.fs = UInt16(value)
          setValid(register)
        case .gs:
          gprs.gs = UInt16(value)
          setValid(register)
        default:
          return
      }
    } else {
      clearValid(register)
    }
  }

  public var description: String {
    return """
      rax: \(hex(gprs.getR(0))) rbx: \(hex(gprs.getR(3))) rcx: \(hex(gprs.getR(2)))
      rdx: \(hex(gprs.getR(1))) rsi: \(hex(gprs.getR(4))) rdi: \(hex(gprs.getR(5)))
      rbp: \(hex(gprs.getR(6))) rsp: \(hex(gprs.getR(7)))  r8: \(hex(gprs.getR(8)))
       r9: \(hex(gprs.getR(9))) r10: \(hex(gprs.getR(10))) r11: \(hex(gprs.getR(11)))
      r12: \(hex(gprs.getR(12))) r13: \(hex(gprs.getR(13))) r14: \(hex(gprs.getR(14)))
      r15: \(hex(gprs.getR(15)))

       cs: \(hex(gprs.cs))  fs: \(hex(gprs.fs))  gs: \(hex(gprs.gs))

      rip: \(hex(gprs.rip)) rflags: \(hex(gprs.rflags))
      """
  }

  public static func isAlignedForStack(framePointer: Address) -> Bool {
    return (framePointer & 0xf) == 0
  }

  #if os(macOS) || os(iOS) || os(watchOS) || os(tvOS)
  internal static var coreSymbolicationArchitecture: CSArchitecture {
    return kCSArchitectureX86_64
  }
  #endif
}

// .. i386 .....................................................................

@_spi(Contexts) public struct I386Context: Context {
  public typealias Address = UInt32
  public typealias Size = UInt32
  public typealias GPRValue = UInt32
  public typealias Register = I386Register

  var gprs = i386_gprs()

  public var architecture: String { "i386" }

  public var programCounter: GPRValue {
    get { return gprs.eip }
    set {
      gprs.eip = newValue
      gprs.valid |= 1 << 15
    }
  }

  public var framePointer: GPRValue {
    get { return gprs.getR(I386Register.ebp.rawValue) }
    set { gprs.setR(I386Register.ebp.rawValue, to: newValue) }
  }

  public var stackPointer: GPRValue {
    get { return gprs.getR(I386Register.esp.rawValue) }
    set { gprs.setR(I386Register.esp.rawValue, to: newValue) }
  }

  public var callFrameAddress: GPRValue {
    get { return stackPointer }
    set { stackPointer = newValue }
  }

  public static var registerCount: Int { return 50 }

  #if os(Linux) && arch(i386)
  init(with mctx: mcontext_t) {
    gprs.setR(I386Register.eax.rawValue, to: UInt32(bitPattern: mctx.gregs.11))
    gprs.setR(I386Register.ecx.rawValue, to: UInt32(bitPattern: mctx.gregs.10))
    gprs.setR(I386Register.edx.rawValue, to: UInt32(bitPattern: mctx.gregs.9))
    gprs.setR(I386Register.ebx.rawValue, to: UInt32(bitPattern: mctx.gregs.8))
    gprs.setR(I386Register.esp.rawValue, to: UInt32(bitPattern: mctx.gregs.7))
    gprs.setR(I386Register.ebp.rawValue, to: UInt32(bitPattern: mctx.gregs.6))
    gprs.setR(I386Register.ebp.rawValue, to: UInt32(bitPattern: mctx.gregs.5))
    gprs.setR(I386Register.ebp.rawValue, to: UInt32(bitPattern: mctx.gregs.4))
    gprs.eip = UInt32(bitPattern: mctx.gregs.14)
    gprs.eflags = UInt32(bitPattern: mctx.gregs.16)
    gprs.segreg.0 = UInt16(bitPattern: mctx.gregs.2 & 0xffff)  // es
    gprs.segreg.1 = UInt16(bitPattern: mctx.gregs.15 & 0xffff) // cs
    gprs.segreg.2 = UInt16(bitPattern: mctx.gregs.18 & 0xffff) // ss
    gprs.segreg.3 = UInt16(bitPattern: mctx.gregs.3 & 0xffff)  // ds
    gprs.segreg.4 = UInt16(bitPattern: mctx.gregs.1 & 0xffff)  // fs
    gprs.segreg.5 = UInt16(bitPattern: mctx.gregs.0 & 0xffff)  // gs
    gprs.valid = 0x7fff
  }

  public static func fromHostMContext(_ mcontext: Any) -> HostContext {
    return I386Context(with: mcontext as! mcontext_t)
  }
  #endif

  #if os(Windows) || !SWIFT_ASM_AVAILABLE
  struct NotImplemented: Error {}
  public static func withCurrentContext<T>(fn: (I386Context) throws -> T) throws -> T {
    throw NotImplemented()
  }
  #elseif arch(i386)
  @usableFromInline
  @_silgen_name("_swift_get_cpu_context")
  static func _swift_get_cpu_context() -> I386Context

  @_transparent
  public static func withCurrentContext<T>(fn: (I386Context) throws -> T) rethrows -> T {
    return try fn(_swift_get_cpu_context())
  }
  #endif

  private func validNdx(_ register: Register) -> Int? {
    switch register {
      case .eax ... .edi:
        return register.rawValue
      case .eflags:
        return 8
      case .es, .cs, .ss, .ds, .fs, .gs:
        return 9 + register.rawValue - Register.es.rawValue
      case .ra:
        return 15
      default:
        return nil
    }
  }

  private func isValid(_ register: Register) -> Bool {
    guard let ndx = validNdx(register) else {
      return false
    }
    return (gprs.valid & (UInt32(1) << ndx)) != 0
  }

  private mutating func setValid(_ register: Register) {
    guard let ndx = validNdx(register) else {
      return
    }
    gprs.valid |= UInt32(1) << ndx
  }

  private mutating func clearValid(_ register: Register) {
    guard let ndx = validNdx(register) else {
      return
    }
    gprs.valid &= ~(UInt32(1) << ndx)
  }

  public func getRegister(_ register: Register) -> GPRValue? {
    if !isValid(register) {
      return nil
    }
    switch register {
      case .eax ... .edi:
        return gprs.getR(register.rawValue)
      case .eflags: return gprs.eflags
      case .es ... .gs:
        return withUnsafeBytes(of: gprs.segreg) { ptr in
          return ptr.withMemoryRebound(to: GPRValue.self) { regs in
            return regs[register.rawValue - Register.es.rawValue]
          }
        }
      case .ra: return gprs.eip
      default:
        return nil
    }
  }

  public mutating func setRegister(_ register: Register, to value: GPRValue?) {
    if let value = value {
      switch register {
        case .eax ... .edi:
          gprs.setR(register.rawValue, to: value)
        case .eflags:
          gprs.eflags = value
          setValid(register)
        case .es ... .gs:
          withUnsafeMutableBytes(of: &gprs.segreg) { ptr in
            ptr.withMemoryRebound(to: GPRValue.self) { regs in
              regs[register.rawValue - Register.es.rawValue] = value
            }
          }
          setValid(register)
        case .ra:
          gprs.eip = value
          setValid(register)
        default:
          return
      }
    } else {
      clearValid(register)
    }
  }

  public var description: String {
    return """
      eax: \(hex(gprs.getR(0))) ebx: \(hex(gprs.getR(3))) ecx: \(hex(gprs.getR(1))) edx: \(hex(gprs.getR(2)))
      esi: \(hex(gprs.getR(6))) edi: \(hex(gprs.getR(7))) ebp: \(hex(gprs.getR(5))) esp: \(hex(gprs.getR(4)))

      es: \(hex(gprs.segreg.0)) cs: \(hex(gprs.segreg.1)) ss: \(hex(gprs.segreg.2)) ds: \(hex(gprs.segreg.3)) fs: \(hex(gprs.segreg.4)) gs: \(hex(gprs.segreg.5))

      eip: \(hex(gprs.eip)) eflags: \(hex(gprs.eflags))
      """
  }

  public static func isAlignedForStack(framePointer: Address) -> Bool {
    return (framePointer & 0xf) == 8
  }

  #if os(macOS) || os(iOS) || os(watchOS) || os(tvOS)
  internal static var coreSymbolicationArchitecture: CSArchitecture {
    return kCSArchitectureI386
  }
  #endif
}

// .. ARM64 ....................................................................

@_spi(Contexts) public struct ARM64Context: Context {
  public typealias Address = UInt64
  public typealias Size = UInt64
  public typealias GPRValue = UInt64
  public typealias Register = ARM64Register

  var gprs = arm64_gprs()

  public var architecture: String { "arm64" }

  public var programCounter: GPRValue {
    get { return gprs.pc }
    set {
      gprs.pc = newValue
      gprs.valid |= 1 << 32
    }
  }

  public var stackPointer: GPRValue {
    get { return gprs.getX(ARM64Register.sp.rawValue) }
    set {
      gprs.setX(ARM64Register.sp.rawValue, to: newValue)
    }
  }

  public var framePointer: GPRValue {
    get { return gprs.getX(ARM64Register.x29.rawValue) }
    set {
      gprs.setX(ARM64Register.x29.rawValue, to: newValue)
    }
  }

  public var callFrameAddress: GPRValue {
    get { return stackPointer }
    set { stackPointer = newValue }
  }

  public static var registerCount: Int { return 40 }

  #if os(macOS) && arch(arm64)
  init?(from thread: thread_t) {
    var state = darwin_arm64_thread_state()
    let kr = thread_get_state(thread,
                              ARM_THREAD_STATE64,
                              &state)
    if kr != KERN_SUCCESS {
      return nil
    }

    self.init(from: state)
  }

  init(with mctx: darwin_arm64_mcontext) {
    self.init(from: mctx.ss)
  }

  init(from state: darwin_arm64_thread_state) {
    withUnsafeMutablePointer(to: &gprs._x) {
      $0.withMemoryRebound(to: UInt64.self, capacity: 32){ to in
        withUnsafePointer(to: state._x) {
          $0.withMemoryRebound(to: UInt64.self, capacity: 29){ from in
            for n in 0..<29 {
              to[n] = from[n]
            }
          }
        }

        to[29] = state.fp
        to[30] = state.lr
        to[31] = state.sp
      }
    }
    gprs.pc = state.pc
    gprs.valid = 0x1ffffffff
  }

  public static func fromHostThread(_ thread: Any) -> HostContext? {
    return ARM64Context(from: thread as! thread_t)
  }

  public static func fromHostMContext(_ mcontext: Any) -> HostContext {
    return ARM64Context(with: mcontext as! darwin_arm64_mcontext)
  }
  #elseif os(Linux) && arch(arm64)
  init(with mctx: mcontext_t) {
    withUnsafeMutablePointer(to: &gprs._x) {
      $0.withMemoryRebound(to: UInt64.self, capacity: 32){ to in
        withUnsafePointer(to: mctx.regs) {
          $0.withMemoryRebound(to: UInt64.self, capacity: 31) { from in
            for n in 0..<31 {
              to[n] = from[n]
            }
          }
        }

	to[31] = mctx.sp
      }
    }
    gprs.pc = mctx.pc
    gprs.valid = 0x1ffffffff
  }

  public static func fromHostMContext(_ mcontext: Any) -> HostContext {
    return ARM64Context(with: mcontext as! mcontext_t)
  }
  #endif

  #if os(Windows) || !SWIFT_ASM_AVAILABLE
  struct NotImplemented: Error {}
  public static func withCurrentContext<T>(fn: (ARM64Context) throws -> T) throws -> T {
    throw NotImplemented()
  }
  #elseif arch(arm64) || arch(arm64_32)
  @usableFromInline
  @_silgen_name("_swift_get_cpu_context")
  static func _swift_get_cpu_context() -> ARM64Context

  @_transparent
  public static func withCurrentContext<T>(fn: (ARM64Context) throws -> T) rethrows -> T {
    return try fn(_swift_get_cpu_context())
  }
  #endif

  private func isValid(_ register: Register) -> Bool {
    if register.rawValue < 33 {
      return (gprs.valid & (UInt64(1) << register.rawValue)) != 0
    }
    return false
  }

  private mutating func setValid(_ register: Register) {
    if register.rawValue < 33 {
      gprs.valid |= UInt64(1) << register.rawValue
    }
  }

  private mutating func clearValid(_ register: Register) {
    if register.rawValue < 33 {
      gprs.valid &= ~(UInt64(1) << register.rawValue)
    }
  }

  public func getRegister(_ reg: Register) -> GPRValue? {
    if !isValid(reg) {
      return nil
    }
    switch reg {
      case .x0 ... .sp:
        return gprs.getX(reg.rawValue)
      case .pc:
        return gprs.pc
      default:
        return nil
    }
  }

  public mutating func setRegister(_ reg: Register, to value: GPRValue?) {
    if let value = value {
      switch reg {
        case .x0 ... .sp:
          gprs.setX(reg.rawValue, to: value)
        case .pc:
          gprs.pc = value
          setValid(reg)
        default:
          break
      }
    } else {
      clearValid(reg)
    }
  }

  public var description: String {
    return """
       x0: \(hex(gprs.getX(0)))  x1: \(hex(gprs.getX(1)))
       x2: \(hex(gprs.getX(2)))  x3: \(hex(gprs.getX(3)))
       x4: \(hex(gprs.getX(4)))  x5: \(hex(gprs.getX(5)))
       x6: \(hex(gprs.getX(6)))  x7: \(hex(gprs.getX(7)))
       x8: \(hex(gprs.getX(8)))  x9: \(hex(gprs.getX(9)))
      x10: \(hex(gprs.getX(10))) x11: \(hex(gprs.getX(11)))
      x12: \(hex(gprs.getX(12))) x13: \(hex(gprs.getX(13)))
      x14: \(hex(gprs.getX(14))) x15: \(hex(gprs.getX(15)))
      x16: \(hex(gprs.getX(16))) x17: \(hex(gprs.getX(17)))
      x18: \(hex(gprs.getX(18))) x19: \(hex(gprs.getX(19)))
      x20: \(hex(gprs.getX(20))) x21: \(hex(gprs.getX(21)))
      x22: \(hex(gprs.getX(22))) x23: \(hex(gprs.getX(23)))
      x24: \(hex(gprs.getX(24))) x25: \(hex(gprs.getX(25)))
      x26: \(hex(gprs.getX(26))) x27: \(hex(gprs.getX(27)))
      x28: \(hex(gprs.getX(28)))

      fp: \(hex(gprs.getX(29))) (aka x29)
      lr: \(hex(gprs.getX(30))) (aka x30)
      sp: \(hex(gprs.getX(31))) (aka x31)

      pc: \(hex(gprs.pc))
      """
  }

  public static func isAlignedForStack(framePointer: Address) -> Bool {
    return (framePointer & 1) == 0
  }

  #if os(macOS) || os(iOS) || os(watchOS) || os(tvOS)
  public static func stripPtrAuth(address: Address) -> Address {
    // Is there a better way to do this?  It'd be easy if we just wanted to
    // strip for the *host*, but we might conceivably want this under other
    // circumstances too.
    return address & 0x00007fffffffffff
  }

  internal static var coreSymbolicationArchitecture: CSArchitecture {
    return kCSArchitectureArm64
  }
  #endif
}

// .. 32-bit ARM ...............................................................

@_spi(Contexts) public struct ARMContext: Context {
  public typealias Address = UInt32
  public typealias Size = UInt32
  public typealias GPRValue = UInt32
  public typealias Register = ARMRegister

  var gprs = arm_gprs()

  public var architecture: String { "arm" }

  public var programCounter: GPRValue {
    get { return gprs.getR(ARMRegister.r15.rawValue) }
    set { gprs.setR(ARMRegister.r15.rawValue, to: newValue) }
  }

  public var stackPointer: GPRValue {
    get { return gprs.getR(ARMRegister.r13.rawValue) }
    set { gprs.setR(ARMRegister.r13.rawValue, to: newValue) }
  }

  public var framePointer: GPRValue {
    get { return gprs.getR(ARMRegister.r11.rawValue) }
    set { gprs.setR(ARMRegister.r11.rawValue, to: newValue) }
  }

  public var callFrameAddress: GPRValue {
    get { return stackPointer }
    set { stackPointer = newValue }
  }

  public static var registerCount: Int { return 16 }

  #if os(Linux) && arch(arm)
  init(with mctx: mcontext_t) {
    withUnsafeMutablePointer(to: &gprs._r) {
      $0.withMemoryRebound(to: UInt32.self, capacity: 16) {
        withUnsafePointer(to: &mctx.arm_r0) {
          $0.withMemoryRebound(to: UInt32.self, capacity: 16) {
            for n in 0..<16 {
              to[n] = from[n]
            }
          }
        }
      }
    }
    gprs.valid = 0xffff
  }

  public static func fromHostMContext(_ mcontext: Any) -> HostContext {
    return ARMContext(with: mcontext as! mcontext_t)
  }
  #endif

  #if os(Windows) || !SWIFT_ASM_AVAILABLE
  struct NotImplemented: Error {}
  public static func withCurrentContext<T>(fn: (ARMContext) throws -> T) throws -> T {
    throw NotImplemented()
  }
  #elseif arch(arm)
  @usableFromInline
  @_silgen_name("_swift_get_cpu_context")
  static func _swift_get_cpu_context() -> ARMContext

  @_transparent
  public static func withCurrentContext<T>(fn: (ARMContext) throws -> T) rethrows -> T {
    return try fn(_swift_get_cpu_context())
  }
  #endif

  private func isValid(_ register: Register) -> Bool {
    if register.rawValue < 16 {
      return (gprs.valid & (UInt32(1) << register.rawValue)) != 0
    }
    return false
  }

  private mutating func setValid(_ register: Register) {
    if register.rawValue < 16 {
      gprs.valid |= UInt32(1) << register.rawValue
    }
  }

  private mutating func clearValid(_ register: Register) {
    if register.rawValue < 16 {
      gprs.valid &= ~(UInt32(1) << register.rawValue)
    }
  }

  public func getRegister(_ reg: Register) -> GPRValue? {
    if !isValid(reg) {
      return nil
    }
    switch reg {
      case .r0 ... .r15:
        return gprs.getR(reg.rawValue)
      default:
        return nil
    }
  }

  public mutating func setRegister(_ reg: Register, to value: GPRValue?) {
    if let value = value {
      switch reg {
        case .r0 ... .r15:
          gprs.setR(reg.rawValue, to: value)
        default:
          break
      }
    } else {
      clearValid(reg)
    }
  }

  public var description: String {
    return """
       r0: \(hex(gprs.getR(0)))  r1: \(hex(gprs.getR(1)))
       r2: \(hex(gprs.getR(2)))  r3: \(hex(gprs.getR(3)))
       r4: \(hex(gprs.getR(4)))  r5: \(hex(gprs.getR(5)))
       r6: \(hex(gprs.getR(6)))  r7: \(hex(gprs.getR(7)))
       r8: \(hex(gprs.getR(8)))  r9: \(hex(gprs.getR(9)))
      r10: \(hex(gprs.getR(10)))

       fp: \(hex(gprs.getR(11))) (aka r11)
       ip: \(hex(gprs.getR(12))) (aka r12)
       sp: \(hex(gprs.getR(13))) (aka r13)
       lr: \(hex(gprs.getR(14))) (aka r14)
       pc: \(hex(gprs.getR(15))) (aka r15)
      """
  }

  public static func isAlignedForStack(framePointer: Address) -> Bool {
    return (framePointer & 1) == 0
  }

  #if os(macOS) || os(iOS) || os(watchOS) || os(tvOS)
  internal static var coreSymbolicationArchitecture: CSArchitecture {
    return kCSArchitectureArmV7K
  }
  #endif
}

// .. Darwin specifics .........................................................

#if (os(macOS) || os(iOS) || os(watchOS) || os(tvOS))
private func thread_get_state<T>(_ thread: thread_t,
                                 _ flavor: CInt,
                                 _ result: inout T) -> kern_return_t {
  var count: mach_msg_type_number_t
    = mach_msg_type_number_t(MemoryLayout<T>.stride
                               / MemoryLayout<natural_t>.stride)

  return withUnsafeMutablePointer(to: &result) { ptr in
    ptr.withMemoryRebound(to: natural_t.self,
                          capacity: Int(count)) { intPtr in
      return thread_get_state(thread,
                              thread_state_flavor_t(flavor),
                              intPtr,
                              &count)
    }
  }
}
#endif

// .. HostContext ..............................................................

/// HostContext is an alias for the appropriate context for the machine on which
/// the code was compiled.
#if arch(x86_64)
@_spi(Contexts) public typealias HostContext = X86_64Context
#elseif arch(i386)
@_spi(Contexts) public typealias HostContext = I386Context
#elseif arch(arm64) || arch(arm64_32)
@_spi(Contexts) public typealias HostContext = ARM64Context
#elseif arch(arm)
@_spi(Contexts) public typealias HostContext = ARMContext
#endif
