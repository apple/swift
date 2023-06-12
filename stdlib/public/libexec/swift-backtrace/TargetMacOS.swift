//===--- TargetMacOS.swift - Represents a process we are inspecting -------===//
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
//  Defines `Target`, which represents the process we are inspecting.
//  This is the macOS version.
//
//===----------------------------------------------------------------------===//

#if os(macOS)

import Darwin
import Darwin.Mach

import _Backtracing
@_spi(Internal) import _Backtracing
@_spi(Contexts) import _Backtracing
@_spi(MemoryReaders) import _Backtracing

@_implementationOnly import Runtime
@_implementationOnly import OS.Darwin

#if arch(x86_64)
typealias MContext = darwin_x86_64_mcontext
#elseif arch(arm64) || arch(arm64_32)
typealias MContext = darwin_arm64_mcontext
#else
#error("You need to define MContext for this architecture")
#endif

extension thread_extended_info {
  var pth_swiftName: String {
    withUnsafePointer(to: pth_name) { ptr in
      let len = strnlen(ptr, Int(MAXTHREADNAMESIZE))
      return String(decoding: UnsafeRawBufferPointer(start: ptr, count: Int(len)),
                    as: UTF8.self)
    }
  }
}

struct TargetThread {
  typealias ThreadID = UInt64

  var id: ThreadID
  var context: HostContext?
  var name: String
  var backtrace: SymbolicatedBacktrace
}

class Target {
  typealias Address = UInt64

  var pid: pid_t
  var name: String
  var signal: UInt64
  var faultAddress: Address
  var crashingThread: TargetThread.ThreadID

  var task: task_t
  var images: [Backtrace.Image] = []
  var sharedCacheInfo: Backtrace.SharedCacheInfo?

  var threads: [TargetThread] = []
  var crashingThreadNdx: Int = -1

  var signalName: String {
    switch signal {
      case UInt64(SIGQUIT): return "SIGQUIT"
      case UInt64(SIGABRT): return "SIGABRT"
      case UInt64(SIGBUS):  return "SIGBUS"
      case UInt64(SIGFPE): return "SIGFPE"
      case UInt64(SIGILL): return "SIGILL"
      case UInt64(SIGSEGV): return "SIGSEGV"
      case UInt64(SIGTRAP): return "SIGTRAP"
      default: return "\(signal)"
    }
  }

  var signalDescription: String {
    switch signal {
      case UInt64(SIGQUIT): return "Terminated"
      case UInt64(SIGABRT): return "Aborted"
      case UInt64(SIGBUS): return "Bus error"
      case UInt64(SIGFPE): return "Floating point exception"
      case UInt64(SIGILL): return "Illegal instruction"
      case UInt64(SIGSEGV): return "Bad pointer dereference"
      case UInt64(SIGTRAP): return "System trap"
      default:
        return "Signal \(signal)"
    }
  }

  var reader: RemoteMemoryReader

  var mcontext: MContext

  static func getTask(pid: pid_t) -> task_t? {
    var port: task_t = 0
    let kr = task_read_for_pid(mach_task_self_, pid, &port)
    if kr != KERN_SUCCESS {
      return nil
    }
    return port
  }

  static func getProcessName(pid: pid_t) -> String {
    let buffer = UnsafeMutableBufferPointer<UInt8>.allocate(capacity: 4096)
    defer {
      buffer.deallocate()
    }
    let ret = proc_name(pid, buffer.baseAddress, UInt32(buffer.count))
    if ret <= 0 {
      return "<unknown>"
    } else {
      return String(decoding: buffer[0..<Int(ret)], as: UTF8.self)
    }
  }

  static func isPlatformBinary(pid: pid_t) -> Bool {
    var flags = UInt32(0)

    return csops(pid,
                 UInt32(CS_OPS_STATUS),
                 &flags,
                 MemoryLayout<UInt32>.size) != 0 ||
      (flags & UInt32(CS_PLATFORM_BINARY | CS_PLATFORM_PATH)) != 0
  }

  init(crashInfoAddr: UInt64, limit: Int?, top: Int, cache: Bool) {
    pid = getppid()

    if Self.isPlatformBinary(pid: pid) {
      /* Exit silently in this case; either

         1. We can't call csops(), because we're sandboxed, or
         2. The target is a platform binary.

         If we get killed, that is also fine. */
      exit(1)
    }

    // This will normally only succeed if the parent process has
    // the com.apple.security.get-task-allow privilege.  That gets set
    // automatically if you're developing in Xcode; if you're developing
    // on the command line or using SwiftPM, you will need to code sign
    // your binary with that entitlement to get this to work.
    guard let parentTask = Self.getTask(pid: pid) else {
      exit(1)
    }

    task = parentTask

    reader = RemoteMemoryReader(task: task_t(task))

    name = Self.getProcessName(pid: pid)

    let crashInfo: CrashInfo
    do {
      crashInfo = try reader.fetch(from: crashInfoAddr, as: CrashInfo.self)
    } catch {
      print("swift-backtrace: unable to fetch crash info.", to: &standardError)
      exit(1)
    }

    crashingThread = crashInfo.crashing_thread
    signal = crashInfo.signal
    faultAddress = crashInfo.fault_address

    guard let mctx: MContext = try? reader.fetch(from: crashInfo.mctx,
                                                 as: MContext.self) else {
      print("swift-backtrace: unable to fetch mcontext.", to: &standardError)
      exit(1)
    }

    mcontext = mctx

    images = Backtrace.captureImages(for: task)
    sharedCacheInfo = Backtrace.captureSharedCacheInfo(for: task)

    fetchThreads(limit: limit, top: top, cache: cache)
  }

  func fetchThreads(limit: Int?, top: Int, cache: Bool) {
    var threadPorts: thread_act_array_t? = nil
    var threadCount: mach_msg_type_number_t = 0
    let kr = task_threads(task,
                          &threadPorts,
                          &threadCount)

    if kr != KERN_SUCCESS {
      print("swift-backtrace: failed to enumerate threads - \(kr)",
            to: &standardError)
      exit(1)
    }

    guard let ports = threadPorts else {
      print("swift-backtrace: thread array is nil", to: &standardError)
      exit(1)
    }

    for ndx in 0..<threadCount {
      var threadIdInfo: thread_identifier_info_data_t? = nil
      var kr = mach_thread_info(ports[Int(ndx)], THREAD_IDENTIFIER_INFO,
                                &threadIdInfo)
      if kr != KERN_SUCCESS {
        print("swift-backtrace: unable to get thread info for thread \(ndx) - \(kr)",
              to: &standardError)
        exit(1)
      }

      guard let info = threadIdInfo else {
        print("swift-backtrace: thread info is nil", to: &standardError)
        exit(1)
      }

      var extInfo = thread_extended_info_data_t()

      let threadName: String

      kr = mach_thread_info(ports[Int(ndx)],
                            THREAD_EXTENDED_INFO,
                            &extInfo)
      if kr == KERN_SUCCESS {
        threadName = extInfo.pth_swiftName
      } else {
        print("swift-backtrace: unable to fetch ext info \(kr)",
              to: &standardError)
        threadName = ""
      }

      let ctx: HostContext

      if info.thread_id == crashingThread {
        ctx = HostContext.fromHostMContext(mcontext)
        crashingThreadNdx = Int(ndx)
      } else {
        guard let threadCtx = HostContext.fromHostThread(ports[Int(ndx)]) else {
          // This can happen legitimately, e.g. when looking at a Rosetta 2
          // process, where there are ARM64 threads AS WELL AS the x86_64 ones.
          continue
        }
        ctx = threadCtx
      }

      guard let backtrace = try? Backtrace.capture(from: ctx,
                                                   using: reader,
                                                   images: nil,
                                                   limit: limit,
                                                   top: top) else {
        print("swift-backtrace: unable to capture backtrace from context for thread \(ndx)",
              to: &standardError)
        exit(1)
      }

      guard let symbolicated = backtrace.symbolicated(with: images,
                                                      sharedCacheInfo: sharedCacheInfo,
                                                      useSymbolCache: cache) else {
        print("unable to symbolicate backtrace from context for thread \(ndx)",
              to: &standardError)
        exit(1)
      }

      threads.append(TargetThread(id: info.thread_id,
                                  context: ctx,
                                  name: threadName,
                                  backtrace: symbolicated))

      mach_port_deallocate(mach_task_self_, ports[Int(ndx)])
    }
  }

  public func redoBacktraces(limit: Int?, top: Int, cache: Bool) {
    for (ndx, thread) in threads.enumerated() {
      guard let context = thread.context else {
        continue
      }

      guard let backtrace = try? Backtrace.capture(from: context,
                                                   using: reader,
                                                   images: nil,
                                                   limit: limit,
                                                   top: top) else {
        print("swift-backtrace: unable to capture backtrace from context for thread \(ndx)",
              to: &standardError)
        continue
      }

      guard let symbolicated = backtrace.symbolicated(with: images,
                                                      sharedCacheInfo: sharedCacheInfo,
                                                      useSymbolCache: cache) else {
        print("swift-backtrace: unable to symbolicate backtrace from context for thread \(ndx)",
              to: &standardError)
        continue
      }

      threads[ndx].backtrace = symbolicated
    }
  }

  public func withDebugger(_ body: () -> ()) throws {
    #if os(macOS)
    return try withTemporaryDirectory(pattern: "/tmp/backtrace.XXXXXXXX") {
      tmpdir in

      let cmdfile = "\(tmpdir)/lldb.command"
      guard let fp = fopen(cmdfile, "wt") else {
        throw PosixError(errno: errno)
      }
      if fputs("""
                 #!/bin/bash
                 clear
                 echo "Once LLDB has attached, return to the other window and press any key"
                 echo ""
                 xcrun lldb --attach-pid \(pid) -o c
                 """, fp) == EOF {
        throw PosixError(errno: errno)
      }
      if fclose(fp) != 0 {
        throw PosixError(errno: errno)
      }
      if chmod(cmdfile, S_IXUSR|S_IRUSR) != 0 {
        throw PosixError(errno: errno)
      }

      try spawn("/usr/bin/open", args: ["open", cmdfile])

      body()
    }
    #else
    print("""
            From another shell, please run

            lldb --attach-pid \(target.pid) -o c
            """)
    body()
    #endif
  }
}

private func mach_thread_info<T>(_ thread: thread_t,
                                 _ flavor: CInt,
                                 _ result: inout T) -> kern_return_t {
  var count: mach_msg_type_number_t
    = mach_msg_type_number_t(MemoryLayout<T>.stride
                               / MemoryLayout<natural_t>.stride)

  return withUnsafeMutablePointer(to: &result) { ptr in
    ptr.withMemoryRebound(to: natural_t.self, capacity: Int(count)) { intPtr in
      return thread_info(thread,
                         thread_flavor_t(flavor),
                         intPtr,
                         &count)
    }
  }
}

#endif // os(macOS)
