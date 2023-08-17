//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#if os(Windows)

import WinSDK
import SwiftRemoteMirror
import Foundation
import SwiftInspectClientInterface

internal final class WindowsRemoteProcess: RemoteProcess {
  public typealias ProcessIdentifier = DWORD
  public typealias ProcessHandle = HANDLE

  public private(set) var process: ProcessHandle
  public private(set) var context: SwiftReflectionContextRef!

  private var hSwiftCore: HMODULE = HMODULE(bitPattern: -1)!

  static var QueryDataLayout: QueryDataLayoutFunction {
    return { (context, type, _, output) in
      let _ = WindowsRemoteProcess.fromOpaque(context!)

      switch type {
      case DLQ_GetPointerSize:
        let size = UInt8(MemoryLayout<UnsafeRawPointer>.stride)
        output?.storeBytes(of: size, toByteOffset: 0, as: UInt8.self)
        return 1

      case DLQ_GetSizeSize:
        // FIXME(compnerd) support 32-bit processes
        let size = UInt8(MemoryLayout<UInt64>.stride)
        output?.storeBytes(of: size, toByteOffset: 0, as: UInt8.self)
        return 1

      case DLQ_GetLeastValidPointerValue:
        let value: UInt64 = 0x1000
        output?.storeBytes(of: value, toByteOffset: 0, as: UInt64.self)
        return 1

      default:
        return 0
      }
    }
  }

  static var Free: FreeFunction {
    return { (_, bytes, _) in
      free(UnsafeMutableRawPointer(mutating: bytes))
    }
  }

  static var ReadBytes: ReadBytesFunction {
    return { (context, address, size, _) in
      let process: WindowsRemoteProcess =
        WindowsRemoteProcess.fromOpaque(context!)

      guard let buffer = malloc(Int(size)) else { return nil }
      if !ReadProcessMemory(
        process.process, LPVOID(bitPattern: UInt(address)),
        buffer, size, nil)
      {
        free(buffer)
        return nil
      }
      return UnsafeRawPointer(buffer)
    }
  }

  static var GetStringLength: GetStringLengthFunction {
    return { (context, address) in
      let process: WindowsRemoteProcess =
        WindowsRemoteProcess.fromOpaque(context!)

      var information: WIN32_MEMORY_REGION_INFORMATION =
        WIN32_MEMORY_REGION_INFORMATION()
      if !QueryVirtualMemoryInformation(
        process.process,
        LPVOID(bitPattern: UInt(address)),
        MemoryRegionInfo, &information,
        SIZE_T(MemoryLayout.size(ofValue: information)),
        nil)
      {
        return 0
      }

      // FIXME(compnerd) mapping in the memory region from the remote process
      // would be ideal to avoid a round-trip for each byte.  This seems to work
      // well enough for now in practice, but we should fix this to provide a
      // proper remote `strlen` implementation.
      //
      // Read 64-bytes, though limit it to the size of the memory region.
      let length: Int = Int(
        min(
          UInt(information.RegionSize)
            - (UInt(address) - UInt(bitPattern: information.AllocationBase)), 64))
      let string: String = [CChar](unsafeUninitializedCapacity: length) {
        $1 = 0
        var NumberOfBytesRead: SIZE_T = 0
        if ReadProcessMemory(
          process.process, LPVOID(bitPattern: UInt(address)),
          $0.baseAddress, SIZE_T($0.count), &NumberOfBytesRead)
        {
          $1 = Int(NumberOfBytesRead)
        }
      }.withUnsafeBufferPointer {
        String(cString: $0.baseAddress!)
      }

      return UInt64(string.count)
    }
  }

  static var GetSymbolAddress: GetSymbolAddressFunction {
    return { (context, symbol, length) in
      let process: WindowsRemoteProcess =
        WindowsRemoteProcess.fromOpaque(context!)

      guard let symbol = symbol else { return 0 }
      let name: String = symbol.withMemoryRebound(to: UInt8.self, capacity: Int(length)) {
        let buffer = UnsafeBufferPointer(start: $0, count: Int(length))
        return String(decoding: buffer, as: UTF8.self)
      }

      return unsafeBitCast(GetProcAddress(process.hSwiftCore, name), to: swift_addr_t.self)
    }
  }

  init?(processId: ProcessIdentifier) {
    // Get process handle.
    self.process =
      OpenProcess(
        DWORD(
          PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION),
        false,
        processId)

    // Initialize SwiftReflectionContextRef
    guard
      let context =
        swift_reflection_createReflectionContextWithDataLayout(
          self.toOpaqueRef(),
          Self.QueryDataLayout,
          Self.Free,
          Self.ReadBytes,
          Self.GetStringLength,
          Self.GetSymbolAddress)
    else {
      // FIXME(compnerd) log error
      return nil
    }
    self.context = context

    // Locate swiftCore.dll in the target process and load modules.
    modules(of: processId) { (entry, module) in
      // FIXME(compnerd) support static linking at some point
      if module == "swiftCore.dll" { self.hSwiftCore = entry.hModule }
      _ = swift_reflection_addImage(context,
                                    unsafeBitCast(entry.modBaseAddr,
                                                  to: swift_addr_t.self))
    }
    if self.hSwiftCore == HMODULE(bitPattern: -1) {
      // FIXME(compnerd) log error
      return nil
    }

    // Initialize DbgHelp.
    if !SymInitialize(self.process, nil, true) {
      // FIXME(compnerd) log error
      return nil
    }
  }

  deinit {
    swift_reflection_destroyReflectionContext(self.context)
    _ = SymCleanup(self.process)
    _ = CloseHandle(self.process)
    self.release()
  }

  func symbolicate(_ address: swift_addr_t) -> (module: String?, symbol: String?) {
    let kMaxSymbolNameLength: Int = 1024

    let byteCount = MemoryLayout<SYMBOL_INFO>.size + kMaxSymbolNameLength + 1

    let buffer: UnsafeMutableRawPointer =
      UnsafeMutableRawPointer.allocate(byteCount: byteCount, alignment: 1)
    defer { buffer.deallocate() }

    let pSymbolInfo: UnsafeMutablePointer<SYMBOL_INFO> =
      buffer.bindMemory(to: SYMBOL_INFO.self, capacity: 1)
    pSymbolInfo.pointee.SizeOfStruct = ULONG(MemoryLayout<SYMBOL_INFO>.size)
    pSymbolInfo.pointee.MaxNameLen = ULONG(kMaxSymbolNameLength)

    guard SymFromAddr(self.process, DWORD64(address), nil, pSymbolInfo) else {
      return (nil, nil)
    }

    let symbol: String = withUnsafePointer(to: &pSymbolInfo.pointee.Name) {
      String(cString: $0)
    }

    var context: (DWORD64, String?) = (pSymbolInfo.pointee.ModBase, nil)
    _ = withUnsafeMutablePointer(to: &context) {
      SymEnumerateModules64(self.process, { (ModuleName, BaseOfDll, UserContext) -> WindowsBool in
        if let pContext = UserContext?.bindMemory(to: (DWORD64, String?).self, capacity: 1) {
          if pContext.pointee.0 == BaseOfDll {
            pContext.pointee.1 = String(cString: ModuleName!)
            return false
          }
        }
        return true
      }, $0)
    }

    return (context.1, symbol)
  }

  internal func iterateHeap(_ body: (swift_addr_t, UInt64) -> Void) {
    let dwProcessId: DWORD = GetProcessId(self.process)
    if dwProcessId == 0 {
      // FIXME(compnerd) log error
      return
    }

    // We use a shared memory and two event objects to send heap entries data
    // from the remote process to this process. A high-level structure looks
    // like below:
    //
    // Swift inspect (this process):
    //
    // Setup the shared memory and event objects
    // Create a remote thread to invoke the heap walk on the remote process
    // Loop {
    //   Wait on ReadEvent to wait for heap entries in the shared memory
    //   If no entries, break
    //   Inspect and dump heap entries from the shared memory
    //   Notify (SetEvent) on WriteEvent to have more heap entries written
    // }
    //
    // Remote process:
    //
    // Open the shared memory and event objects
    // Heap walk loop {
    //   Write heap entries in the shared memory until full or done
    //   Notify (SetEvent) ReadEvent to have them read
    //   Wait on WriteEvent until they are read
    // }
    //

    // Exclude the self-inspect case. We use IPC + HeapWalk in the remote
    // process, which doesn't work on itself.
    if dwProcessId == GetCurrentProcessId() {
      print("Cannot inspect the process itself")
      return
    }

    // The size of the shared memory buffer and the names of shared
    // memory and event objects
    let bufSize = Int(BUF_SIZE)
    let sharedMemoryName = "\(SHARED_MEM_NAME_PREFIX)-\(String(dwProcessId))"
    let waitTimeoutMs = DWORD(WAIT_TIMEOUT_MS)

    // Set up the shared memory
    let hMapFile = CreateFileMappingA(
      INVALID_HANDLE_VALUE,
      nil,
      DWORD(PAGE_READWRITE),
      0,
      DWORD(bufSize),
      sharedMemoryName)
    if hMapFile == HANDLE(bitPattern: 0) {
      print("CreateFileMapping failed \(GetLastError())")
      return
    }
    defer { CloseHandle(hMapFile) }
    let buf: LPVOID = MapViewOfFile(
      hMapFile,
      FILE_MAP_ALL_ACCESS,
      0,
      0,
      SIZE_T(bufSize))
    if buf == LPVOID(bitPattern: 0) {
      print("MapViewOfFile failed \(GetLastError())")
      return
    }
    defer { UnmapViewOfFile(buf) }

    // Set up the event objects
    guard let (hReadEvent, hWriteEvent) = createEventPair(dwProcessId) else {
      return
    }
    defer {
      CloseHandle(hReadEvent)
      CloseHandle(hWriteEvent)
    }

    // Allocate the dll path string in the remote process.
    guard let dllPathRemote = allocateDllPathRemote() else {
      return
    }

    // Load the dll and start the heap walk
    guard
      let remoteAddrs = findRemoteAddresses(
        dwProcessId: dwProcessId, moduleName: "KERNEL32.DLL",
        symbols: ["LoadLibraryW", "FreeLibrary"])
    else {
      print("Failed to find remote LoadLibraryW/FreeLibrary addresses")
      return
    }
    let (loadLibraryAddr, pfnFreeLibrary) = (remoteAddrs[0], remoteAddrs[1])
    let hThread: HANDLE = CreateRemoteThread(
      self.process, nil, 0, loadLibraryAddr,
      dllPathRemote, 0, nil)
    if hThread == HANDLE(bitPattern: 0) {
      print("CreateRemoteThread failed \(GetLastError())")
      return
    }
    defer { CloseHandle(hThread) }

    defer {
      // Always perform the code ejection process even if the heap walk fails.
      // The module cannot re-execute the heap walk and will leave a retain
      // count behind, preventing the module from being unlinked on the file
      // system as well as leave code in the inspected process.  This will
      // eventually be an issue for treating the injected code as a resource
      // which is extracted temporarily.
      if !eject(module: dllPathRemote, from: dwProcessId, pfnFreeLibrary) {
        print("Failed to unload the remote dll")
      }
    }

    // The main heap iteration loop.
    outer: while true {
      let wait = WaitForSingleObject(hReadEvent, waitTimeoutMs)
      if wait != WAIT_OBJECT_0 {
        print("WaitForSingleObject failed \(wait)")
        return
      }

      let entryCount = bufSize / MemoryLayout<HeapEntry>.size

      for entry in UnsafeMutableBufferPointer(
        start: buf.bindMemory(
          to: HeapEntry.self,
          capacity: entryCount),
        count: entryCount)
      {
        if entry.Address == UInt.max {
          // The buffer containing zero entries, indicated by the first entry
          // contains -1, means, we are done. Break out of loop.
          if !SetEvent(hWriteEvent) {
            print("SetEvent failed: \(GetLastError())")
            return
          }
          break outer
        }
        if entry.Address == 0 {
          // Done. Break out of loop.
          break
        }
        body(swift_addr_t(entry.Address), UInt64(entry.Size))
      }

      if !SetEvent(hWriteEvent) {
        print("SetEvent failed \(GetLastError())")
        return
      }
    }

    let wait = WaitForSingleObject(hThread, waitTimeoutMs)
    if wait != WAIT_OBJECT_0 {
      print("WaitForSingleObject on LoadLibrary failed \(wait)")
      return
    }

    var threadExitCode: DWORD = 0
    GetExitCodeThread(hThread, &threadExitCode)
    if threadExitCode == 0 {
      print("LoadLibraryW failed \(threadExitCode)")
      return
    }
  }

  private func allocateDllPathRemote() -> UnsafeMutableRawPointer? {
    URL(fileURLWithPath: ProcessInfo.processInfo.arguments[0])
      .deletingLastPathComponent()
      .appendingPathComponent("SwiftInspectClient.dll")
      .path
      .withCString(encodedAs: UTF16.self) { pwszPath in
        let dwLength = GetFullPathNameW(pwszPath, 0, nil, nil)
        return withUnsafeTemporaryAllocation(of: WCHAR.self, capacity: Int(dwLength)) {
          guard GetFullPathNameW(pwszPath, dwLength, $0.baseAddress, nil) == dwLength - 1 else { return nil }

          var faAttributes: WIN32_FILE_ATTRIBUTE_DATA = .init()
          guard GetFileAttributesExW($0.baseAddress, GetFileExInfoStandard, &faAttributes) else {
            print("\(String(decodingCString: $0.baseAddress!, as: UTF16.self)) doesn't exist")
            return nil
          }
          guard faAttributes.dwFileAttributes & DWORD(FILE_ATTRIBUTE_REPARSE_POINT) == 0 else {
            print("\(String(decodingCString: $0.baseAddress!, as: UTF16.self)) doesn't exist")
            return nil
          }

          let szLength = SIZE_T(Int(dwLength) * MemoryLayout<WCHAR>.size)
          guard let pAllocation =
              VirtualAllocEx(self.process, nil, szLength,
                             DWORD(MEM_COMMIT), DWORD(PAGE_READWRITE)) else {
            print("VirtualAllocEx failed \(GetLastError())")
            return nil
          }

          if !WriteProcessMemory(self.process, pAllocation, $0.baseAddress, szLength, nil) {
            print("WriteProcessMemory failed \(GetLastError())")
            _ = VirtualFreeEx(self.process, pAllocation, 0, DWORD(MEM_RELEASE))
            return nil
          }

          return pAllocation
        }
      }
  }

  /// Eject the injected code from the instrumented process.
  ///
  /// Performs the necessary clean up to remove the injected code from the
  /// instrumented process once the heap walk is complete.
  private func eject(module dllPathRemote: UnsafeMutableRawPointer,
                     from dwProcessId: DWORD,
                     _ freeLibraryAddr: LPTHREAD_START_ROUTINE) -> Bool {
    // Get the dll module handle in the remote process to use it to
    // unload it below.

    // GetExitCodeThread returns a DWORD (32-bit) but the HMODULE
    // returned from LoadLibraryW is a 64-bit pointer and may be truncated.
    // So, search for it using the snapshot instead.
    guard let hModule = find(module: "SwiftInspectClient.dll", in: dwProcessId) else {
      print("Failed to find the client dll")
      return false
    }
    // Unload the dll from the remote process
    let hUnloadThread = CreateRemoteThread(
      self.process, nil, 0, freeLibraryAddr,
      UnsafeMutableRawPointer(hModule), 0, nil)
    if hUnloadThread == HANDLE(bitPattern: 0) {
      print("CreateRemoteThread for unload failed \(GetLastError())")
      return false
    }
    defer { CloseHandle(hUnloadThread) }
    let unload_wait = WaitForSingleObject(hUnloadThread, DWORD(WAIT_TIMEOUT_MS))
    if unload_wait != WAIT_OBJECT_0 {
      print("WaitForSingleObject on FreeLibrary failed \(unload_wait)")
      return false
    }
    var unloadExitCode: DWORD = 0
    GetExitCodeThread(hUnloadThread, &unloadExitCode)
    if unloadExitCode == 0 {
      print("FreeLibrary failed")
      return false
    }

    // Deallocate the dll path string in the remote process
    if !VirtualFreeEx(self.process, dllPathRemote, 0, DWORD(MEM_RELEASE)) {
      print("VirtualFreeEx failed GLE=\(GetLastError())")
      return false
    }

    return true
  }

  private func modules(of dwProcessId: DWORD, _ closure: (MODULEENTRY32W, String) -> Void) {
    let hModuleSnapshot: HANDLE =
      CreateToolhelp32Snapshot(DWORD(TH32CS_SNAPMODULE), dwProcessId)
    if hModuleSnapshot == INVALID_HANDLE_VALUE {
      print("CreateToolhelp32Snapshot failed \(GetLastError())")
      return
    }
    defer { CloseHandle(hModuleSnapshot) }
    var entry: MODULEENTRY32W = MODULEENTRY32W()
    entry.dwSize = DWORD(MemoryLayout<MODULEENTRY32W>.size)
    guard Module32FirstW(hModuleSnapshot, &entry) else {
      print("Module32FirstW failed \(GetLastError())")
      return
    }
    repeat {
      let module: String = withUnsafePointer(to: entry.szModule) {
        $0.withMemoryRebound(
          to: WCHAR.self,
          capacity: MemoryLayout.size(ofValue: $0) / MemoryLayout<WCHAR>.size
        ) {
          String(decodingCString: $0, as: UTF16.self)
        }
      }
      closure(entry, module)
    } while Module32NextW(hModuleSnapshot, &entry)
  }

  private func find(module named: String, in dwProcessId: DWORD) -> HMODULE? {
    var hModule: HMODULE?
    modules(of: dwProcessId) { (entry, module) in
      if module == named { hModule = entry.hModule }
    }
    return hModule
  }

  private func findRemoteAddresses(dwProcessId: DWORD, moduleName: String, symbols: [String])
    -> [LPTHREAD_START_ROUTINE]?
  {
    guard let hDllModule = find(module: moduleName, in: dwProcessId) else {
      print("Failed to find remote module \(moduleName)")
      return nil
    }
    var addresses: [LPTHREAD_START_ROUTINE] = []
    for sym in symbols {
      addresses.append(
        unsafeBitCast(GetProcAddress(hDllModule, sym), to: LPTHREAD_START_ROUTINE.self))
    }
    return addresses
  }

  private func createEventPair(_ dwProcessId: DWORD) -> (HANDLE, HANDLE)? {
    let readEventName = READ_EVENT_NAME_PREFIX + "-" + String(dwProcessId)
    let writeEventName = WRITE_EVENT_NAME_PREFIX + "-" + String(dwProcessId)
    let hReadEvent: HANDLE = CreateEventA(
      LPSECURITY_ATTRIBUTES(bitPattern: 0),
      false,  // Auto-reset
      false,  // Initial state is nonsignaled
      readEventName)
    if hReadEvent == HANDLE(bitPattern: 0) {
      print("CreateEvent failed \(GetLastError())")
      return nil
    }
    let hWriteEvent: HANDLE = CreateEventA(
      LPSECURITY_ATTRIBUTES(bitPattern: 0),
      false,  // Auto-reset
      false,  // Initial state is nonsignaled
      writeEventName)
    if hWriteEvent == HANDLE(bitPattern: 0) {
      print("CreateEvent failed \(GetLastError())")
      CloseHandle(hReadEvent)
      return nil
    }
    return (hReadEvent, hWriteEvent)
  }

}

#endif
