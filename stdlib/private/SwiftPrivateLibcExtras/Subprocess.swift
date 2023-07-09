//===--- Subprocess.swift -------------------------------------------------===//
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

import SwiftPrivate
#if canImport(Darwin)
import Darwin
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#elseif os(WASI)
import WASILibc
#elseif os(Windows)
import CRT
import WinSDK
#endif

#if !os(WASI)
// No signals support on WASI yet, see https://github.com/WebAssembly/WASI/issues/166.
internal func _signalToString(_ signal: Int) -> String {
  switch CInt(signal) {
  case SIGILL:  return "SIGILL"
  case SIGABRT: return "SIGABRT"
  case SIGFPE:  return "SIGFPE"
  case SIGSEGV: return "SIGSEGV"
#if !os(Windows)
  case SIGTRAP: return "SIGTRAP"
  case SIGBUS:  return "SIGBUS"
  case SIGSYS:  return "SIGSYS"
#endif
  default:      return "SIG???? (\(signal))"
  }
}
#endif

public enum ProcessTerminationStatus : CustomStringConvertible {
  case exit(Int)
  case signal(Int)

  public var description: String {
    switch self {
    case .exit(let status):
      return "Exit(\(status))"
    case .signal(let signal):
#if os(WASI)
      // No signals support on WASI yet, see https://github.com/WebAssembly/WASI/issues/166.
      fatalError("Signals are not supported on WebAssembly/WASI")
#else
      return "Signal(\(_signalToString(signal)))"
#endif
    }
  }
}

#if !SWIFT_STDLIB_HAS_COMMANDLINE
@_silgen_name("_swift_stdlib_getUnsafeArgvArgc")
internal func _swift_stdlib_getUnsafeArgvArgc(_: UnsafeMutablePointer<Int32>) -> UnsafeMutablePointer<UnsafeMutablePointer<Int8>?>

public enum CommandLine {
  public static var arguments: [String] = {
    var argc: Int32 = 0
    var unsafeArgv = _swift_stdlib_getUnsafeArgvArgc(&argc)
    return (0 ..< Int(argc)).map { String(cString: unsafeArgv[$0]!) }
  }()
}
#endif // !SWIFT_STDLIB_HAS_COMMANDLINE

#if os(Windows)
public func spawnChild(_ args: [String])
    -> (process: HANDLE, stdin: HANDLE, stdout: HANDLE, stderr: HANDLE) {
  var _stdin: (read: HANDLE?, write: HANDLE?)
  var _stdout: (read: HANDLE?, write: HANDLE?)
  var _stderr: (read: HANDLE?, write: HANDLE?)

  var saAttributes: SECURITY_ATTRIBUTES = SECURITY_ATTRIBUTES()
  saAttributes.nLength = DWORD(MemoryLayout<SECURITY_ATTRIBUTES>.size)
  saAttributes.bInheritHandle = true
  saAttributes.lpSecurityDescriptor = nil

  if !CreatePipe(&_stdin.read, &_stdin.write, &saAttributes, 0) {
    fatalError("CreatePipe() failed")
  }
  if !SetHandleInformation(_stdin.write, HANDLE_FLAG_INHERIT, 0) {
    fatalError("SetHandleInformation() failed")
  }

  if !CreatePipe(&_stdout.read, &_stdout.write, &saAttributes, 0) {
    fatalError("CreatePipe() failed")
  }
  if !SetHandleInformation(_stdout.read, HANDLE_FLAG_INHERIT, 0) {
    fatalError("SetHandleInformation() failed")
  }

  if !CreatePipe(&_stderr.read, &_stderr.write, &saAttributes, 0) {
    fatalError("CreatePipe() failed")
  }
  if !SetHandleInformation(_stderr.read, HANDLE_FLAG_INHERIT, 0) {
    fatalError("SetHandleInformation() failed")
  }

  var siStartupInfo: STARTUPINFOW = STARTUPINFOW()
  siStartupInfo.cb = DWORD(MemoryLayout<STARTUPINFOW>.size)
  siStartupInfo.hStdError = _stderr.write
  siStartupInfo.hStdOutput = _stdout.write
  siStartupInfo.hStdInput = _stdin.read
  siStartupInfo.dwFlags |= STARTF_USESTDHANDLES

  var piProcessInfo: PROCESS_INFORMATION = PROCESS_INFORMATION()

  // TODO(compnerd): properly quote the command line being invoked here.  See
  // https://blogs.msdn.microsoft.com/twistylittlepassagesallalike/2011/04/23/everyone-quotes-command-line-arguments-the-wrong-way/
  // for more details on how to properly quote the command line for Windows.
  let command: String =
      ([CommandLine.arguments[0]] + args).joined(separator: " ")
  command.withCString(encodedAs: UTF16.self) { cString in
    if !CreateProcessW(nil, UnsafeMutablePointer<WCHAR>(mutating: cString),
                       nil, nil, true, 0, nil, nil,
                       &siStartupInfo, &piProcessInfo) {
      let dwError: DWORD = GetLastError()
      fatalError("CreateProcessW() failed \(dwError)")
    }
  }

  if !CloseHandle(_stdin.read) {
    fatalError("CloseHandle() failed")
  }
  if !CloseHandle(_stdout.write) {
    fatalError("CloseHandle() failed")
  }
  if !CloseHandle(_stderr.write) {
    fatalError("CloseHandle() failed")
  }

  // CloseHandle(piProcessInfo.hProcess)
  CloseHandle(piProcessInfo.hThread)

  return (piProcessInfo.hProcess,
          _stdin.write ?? INVALID_HANDLE_VALUE,
          _stdout.read ?? INVALID_HANDLE_VALUE,
          _stderr.read ?? INVALID_HANDLE_VALUE)
}

public func waitProcess(_ process: HANDLE) -> ProcessTerminationStatus {
  let result = WaitForSingleObject(process, INFINITE)
  if result != WAIT_OBJECT_0 {
    fatalError("WaitForSingleObject() failed")
  }

  var status: DWORD = 0
  if !GetExitCodeProcess(process, &status) {
    fatalError("GetExitCodeProcess() failed")
  }

  if status & DWORD(0x80000000) == DWORD(0x80000000) {
    return .signal(Int(status))
  }
  return .exit(Int(status))
}
#elseif os(WASI)
// WASI doesn't support child processes
public func spawnChild(_ args: [String])
  -> (pid: pid_t, stdinFD: CInt, stdoutFD: CInt, stderrFD: CInt) {
  fatalError("\(#function) is not supported on WebAssembly/WASI")
}
public func posixWaitpid(_ pid: pid_t) -> ProcessTerminationStatus {
  fatalError("\(#function) is not supported on WebAssembly/WASI")
}
#else
// posix_spawn is not available on Android.
// posix_spawn is not available on Haiku.
#if !os(Android) && !os(Haiku)
// posix_spawn isn't available in the public watchOS SDK, we sneak by the
// unavailable attribute declaration here of the APIs that we need.

// FIXME: Come up with a better way to deal with APIs that are pointers on some
// platforms but not others.
#if os(Linux)
typealias _stdlib_posix_spawn_file_actions_t = posix_spawn_file_actions_t
#else
typealias _stdlib_posix_spawn_file_actions_t = posix_spawn_file_actions_t?
#endif

@_silgen_name("_stdlib_posix_spawn_file_actions_init")
internal func _stdlib_posix_spawn_file_actions_init(
  _ file_actions: UnsafeMutablePointer<_stdlib_posix_spawn_file_actions_t>
) -> CInt

@_silgen_name("_stdlib_posix_spawn_file_actions_destroy")
internal func _stdlib_posix_spawn_file_actions_destroy(
  _ file_actions: UnsafeMutablePointer<_stdlib_posix_spawn_file_actions_t>
) -> CInt

@_silgen_name("_stdlib_posix_spawn_file_actions_addclose")
internal func _stdlib_posix_spawn_file_actions_addclose(
  _ file_actions: UnsafeMutablePointer<_stdlib_posix_spawn_file_actions_t>,
  _ filedes: CInt) -> CInt

@_silgen_name("_stdlib_posix_spawn_file_actions_adddup2")
internal func _stdlib_posix_spawn_file_actions_adddup2(
  _ file_actions: UnsafeMutablePointer<_stdlib_posix_spawn_file_actions_t>,
  _ filedes: CInt,
  _ newfiledes: CInt) -> CInt

@_silgen_name("_stdlib_posix_spawn")
internal func _stdlib_posix_spawn(
  _ pid: UnsafeMutablePointer<pid_t>?,
  _ file: UnsafePointer<Int8>,
  _ file_actions: UnsafePointer<_stdlib_posix_spawn_file_actions_t>?,
  _ attrp: UnsafePointer<posix_spawnattr_t>?,
  _ argv: UnsafePointer<UnsafeMutablePointer<Int8>?>,
  _ envp: UnsafePointer<UnsafeMutablePointer<Int8>?>?) -> CInt
#endif

/// Calls POSIX `pipe()`.
func posixPipe() -> (readFD: CInt, writeFD: CInt) {
  var fds: [CInt] = [ -1, -1 ]
  if pipe(&fds) != 0 {
    preconditionFailure("pipe() failed")
  }
  return (fds[0], fds[1])
}

#if !SWIFT_STDLIB_HAS_ENVIRON
@_silgen_name("_NSGetEnviron")
func _NSGetEnviron() -> UnsafeMutablePointer<UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>>

var environ: UnsafeMutablePointer<UnsafeMutablePointer<CChar>?> {
  #if os(macOS)
  return _NSGetEnviron().pointee
  #elseif os(Windows)
  return __p_environ().pointee
  #elseif os(Linux)
  return __environ
  #else
  #error("unsupported platform")
  #endif
}
#endif

#if SWIFT_STDLIB_STATIC_PRINT
func print(_ s: String) {
  let data = Array("\(s)\n".utf8)
  write(STDOUT_FILENO, data, data.count)
}
#endif

/// Start the same executable as a child process, redirecting its stdout and
/// stderr.
public func spawnChild(_ args: [String])
  -> (pid: pid_t, stdinFD: CInt, stdoutFD: CInt, stderrFD: CInt) {
  // The stdout, stdin, and stderr from the child process will be redirected
  // to these pipes.
  let childStdout = posixPipe()
  let childStdin = posixPipe()
  let childStderr = posixPipe()

#if os(Android) || os(Haiku)
  // posix_spawn isn't available on Android. Instead, we fork and exec.
  // To correctly communicate the exit status of the child process to this
  // (parent) process, we'll use this pipe.
  let childToParentPipe = posixPipe()

  let pid = fork()
  precondition(pid >= 0, "fork() failed")
  if pid == 0 {
    // pid of 0 means we are now in the child process.
    // Capture the output before executing the program.
    close(childStdout.readFD)
    close(childStdin.writeFD)
    close(childStderr.readFD)
    close(childToParentPipe.readFD)
    dup2(childStdout.writeFD, STDOUT_FILENO)
    dup2(childStdin.readFD, STDIN_FILENO)
    dup2(childStderr.writeFD, STDERR_FILENO)

    // Set the "close on exec" flag on the parent write pipe. This will
    // close the pipe if the execve() below successfully executes a child
    // process.
    let closeResult = fcntl(childToParentPipe.writeFD, F_SETFD, FD_CLOEXEC)
    let closeErrno = errno
    precondition(
      closeResult == 0,
      "Could not set the close behavior of the child-to-parent pipe; " +
      "errno: \(closeErrno)")

    // Start the executable. If execve() does not encounter an error, the
    // code after this block will never be executed, and the parent write pipe
    // will be closed.
    withArrayOfCStrings([CommandLine.arguments[0]] + args) {
      execve(CommandLine.arguments[0], $0, environ)
    }

    // If execve() encountered an error, we write the errno encountered to the
    // parent write pipe.
    let errnoSize = MemoryLayout.size(ofValue: errno)
    var execveErrno = errno
    let writtenBytes = withUnsafePointer(to: &execveErrno) {
      write(childToParentPipe.writeFD, UnsafePointer($0), errnoSize)
    }

    let writeErrno = errno
    if writtenBytes > 0 && writtenBytes < errnoSize {
      // We were able to write some of our error, but not all of it.
      // FIXME: Retry in this case.
      preconditionFailure("Unable to write entire error to child-to-parent " +
                          "pipe.")
    } else if writtenBytes == 0 {
      preconditionFailure("Unable to write error to child-to-parent pipe.")
    } else if writtenBytes < 0 {
      preconditionFailure("An error occurred when writing error to " +
                          "child-to-parent pipe; errno: \(writeErrno)")
    }

    // Close the pipe when we're done writing the error.
    close(childToParentPipe.writeFD)
  } else {
    close(childToParentPipe.writeFD)

    // Figure out if the child’s call to execve was successful or not.
    var readfds = _stdlib_fd_set()
    readfds.set(childToParentPipe.readFD)
    var writefds = _stdlib_fd_set()
    var errorfds = _stdlib_fd_set()
    errorfds.set(childToParentPipe.readFD)

    var ret: CInt
    repeat {
      ret = _stdlib_select(&readfds, &writefds, &errorfds, nil)
    } while ret == -1 && errno == EINTR
    if ret <= 0 {
      fatalError("select() returned an error: \(errno)")
    }

    if readfds.isset(childToParentPipe.readFD) || errorfds.isset(childToParentPipe.readFD) {
      var childErrno: CInt = 0
      let readResult: ssize_t = withUnsafeMutablePointer(to: &childErrno) {
        return read(childToParentPipe.readFD, $0, MemoryLayout.size(ofValue: $0.pointee))
      }
      if readResult == 0 {
        // We read an EOF indicating that the child's call to execve was successful.
      } else if readResult < 0 {
        fatalError("read() returned error: \(errno)")
      } else {
        // We read an error from the child.
        print(String(cString: strerror(childErrno)))
        preconditionFailure("execve() failed")
      }
    }

    close(childToParentPipe.readFD)
  }
#else
  var fileActions = _make_posix_spawn_file_actions_t()
  if _stdlib_posix_spawn_file_actions_init(&fileActions) != 0 {
    preconditionFailure("_stdlib_posix_spawn_file_actions_init() failed")
  }

  // Close the write end of the pipe on the child side.
  if _stdlib_posix_spawn_file_actions_addclose(
    &fileActions, childStdin.writeFD) != 0 {
    preconditionFailure("_stdlib_posix_spawn_file_actions_addclose() failed")
  }

  // Remap child's stdin.
  if _stdlib_posix_spawn_file_actions_adddup2(
    &fileActions, childStdin.readFD, STDIN_FILENO) != 0 {
    preconditionFailure("_stdlib_posix_spawn_file_actions_adddup2() failed")
  }

  // Close the read end of the pipe on the child side.
  if _stdlib_posix_spawn_file_actions_addclose(
    &fileActions, childStdout.readFD) != 0 {
    preconditionFailure("_stdlib_posix_spawn_file_actions_addclose() failed")
  }

  // Remap child's stdout.
  if _stdlib_posix_spawn_file_actions_adddup2(
    &fileActions, childStdout.writeFD, STDOUT_FILENO) != 0 {
    preconditionFailure("_stdlib_posix_spawn_file_actions_adddup2() failed")
  }

  // Close the read end of the pipe on the child side.
  if _stdlib_posix_spawn_file_actions_addclose(
    &fileActions, childStderr.readFD) != 0 {
    preconditionFailure("_stdlib_posix_spawn_file_actions_addclose() failed")
  }

  // Remap child's stderr.
  if _stdlib_posix_spawn_file_actions_adddup2(
    &fileActions, childStderr.writeFD, STDERR_FILENO) != 0 {
    preconditionFailure("_stdlib_posix_spawn_file_actions_adddup2() failed")
  }

  var pid: pid_t = -1
  var childArgs = args
  childArgs.insert(CommandLine.arguments[0], at: 0)
  let interpreter = getenv("SWIFT_INTERPRETER")
  if interpreter != nil {
    if let invocation = String(validatingUTF8: interpreter!) {
      childArgs.insert(invocation, at: 0)
    }
  }
  let spawnResult = withArrayOfCStrings(childArgs) {
    _stdlib_posix_spawn(
      &pid, childArgs[0], &fileActions, nil, $0, environ)
  }
  if spawnResult != 0 {
    print(String(cString: strerror(spawnResult)))
    preconditionFailure("_stdlib_posix_spawn() failed")
  }

  if _stdlib_posix_spawn_file_actions_destroy(&fileActions) != 0 {
    preconditionFailure("_stdlib_posix_spawn_file_actions_destroy() failed")
  }
#endif

  // Close the read end of the pipe on the parent side.
  if close(childStdin.readFD) != 0 {
    preconditionFailure("close() failed")
  }

  // Close the write end of the pipe on the parent side.
  if close(childStdout.writeFD) != 0 {
    preconditionFailure("close() failed")
  }

  // Close the write end of the pipe on the parent side.
  if close(childStderr.writeFD) != 0 {
    preconditionFailure("close() failed")
  }

  return (pid, childStdin.writeFD, childStdout.readFD, childStderr.readFD)
}

#if !os(Android) && !os(Haiku)
#if os(Linux)
internal func _make_posix_spawn_file_actions_t()
  -> _stdlib_posix_spawn_file_actions_t {
  return posix_spawn_file_actions_t()
}
#else
internal func _make_posix_spawn_file_actions_t()
  -> _stdlib_posix_spawn_file_actions_t {
  return nil
}
#endif
#endif

public func posixWaitpid(_ pid: pid_t) -> ProcessTerminationStatus {
  var status: CInt = 0
#if os(Cygwin)
  withUnsafeMutablePointer(to: &status) {
    statusPtr in
    let statusPtrWrapper = __wait_status_ptr_t(__int_ptr: statusPtr)
    while waitpid(pid, statusPtrWrapper, 0) < 0 {
      if errno != EINTR {
        preconditionFailure("waitpid() failed")
      }
    }
  }
#else
  while waitpid(pid, &status, 0) < 0 {
    if errno != EINTR {
      preconditionFailure("waitpid() failed")
    }
  }
#endif
  if WIFEXITED(status) {
    return .exit(Int(WEXITSTATUS(status)))
  }
  if WIFSIGNALED(status) {
    return .signal(Int(WTERMSIG(status)))
  }
  preconditionFailure("did not understand what happened to child process")
}

// !os(Windows)
#endif

