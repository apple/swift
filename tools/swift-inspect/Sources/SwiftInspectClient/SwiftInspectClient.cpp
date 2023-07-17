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

#if defined(_WIN32)

#pragma comment(lib, "swiftCore.lib")

#include "../SwiftInspectClientInterface/SwiftInspectClientInterface.h"
#include <assert.h>
#include <memory>
#include <stdio.h>
#include <strsafe.h>
#include <vector>
#include <windows.h>

namespace {

struct ScopedHandle {
  HANDLE Handle;
  explicit ScopedHandle(HANDLE Handle) noexcept : Handle(Handle) {}
  ~ScopedHandle() noexcept {
    if (Handle != NULL && Handle != INVALID_HANDLE_VALUE) {
      CloseHandle(Handle);
    }
  }
  operator HANDLE() const { return Handle; }
};

struct ScopedViewOfFile {
  void *View;
  explicit ScopedViewOfFile(void *View) noexcept : View(View) {}
  ~ScopedViewOfFile() noexcept {
    if (View != NULL) {
      UnmapViewOfFile(View);
    }
  }
  void *get() const { return View; }
  template <typename T> T *as() const { return reinterpret_cast<T *>(View); }
};

struct ScopedHeapLock {
  HANDLE Heap;
  bool Failure = false;
  explicit ScopedHeapLock(HANDLE Heap) noexcept : Heap(Heap) {
    if (!HeapLock(Heap)) {
      OutputDebugStringA("Failed to lock heap\n");
      Failure = true;
    }
  }
  ~ScopedHeapLock() noexcept {
    if (Heap != NULL && !Failure) {
      if (!HeapUnlock(Heap)) {
        OutputDebugStringA("Failed to lock heap\n");
      }
    }
  }
};

} // anonymous namespace

#define BUF_NUM_ENTRIES (BUF_SIZE / sizeof(HeapEntry))

static int heapWalk() {
  // Format the shared mem and event object names
  DWORD Pid = GetCurrentProcessId();
  char SharedMemName[128];
  char ReadEventName[128];
  char WriteEventName[128];
  if (StringCbPrintfA(SharedMemName, sizeof(SharedMemName), "%hS-%lu",
                      SHARED_MEM_NAME_PREFIX, Pid) != S_OK) {
    OutputDebugStringA("StringCbPrintfA for SharedMemName failed\n");
    return 1;
  }
  if (StringCbPrintfA(ReadEventName, sizeof(ReadEventName), "%hS-%lu",
                      READ_EVENT_NAME_PREFIX, Pid) != S_OK) {
    OutputDebugStringA("StringCbPrintfA for ReadEventName failed\n");
    return 1;
  }
  if (StringCbPrintfA(WriteEventName, sizeof(WriteEventName), "%hS-%lu",
                      WRITE_EVENT_NAME_PREFIX, Pid) != S_OK) {
    OutputDebugStringA("StringCbPrintfA for WriteEventName failed\n");
    return 1;
  }

  ScopedHandle MapFile(
      OpenFileMappingA(FILE_MAP_ALL_ACCESS, false, SharedMemName));
  if (MapFile == NULL) {
    OutputDebugStringA("OpenFileMapping failed\n");
    return 1;
  }
  ScopedViewOfFile Buf(
      MapViewOfFile(MapFile.Handle, FILE_MAP_ALL_ACCESS, 0, 0, BUF_SIZE));
  if (Buf.get() == NULL) {
    OutputDebugStringA("MapViewOfFile failed\n");
    return 1;
  }
  std::memset(Buf.get(), 0, BUF_SIZE);
  ScopedHandle WriteEvent(OpenEventA(EVENT_ALL_ACCESS, false, WriteEventName));
  if (WriteEvent == NULL) {
    OutputDebugStringA("OpenEventA failed\n");
    return 1;
  }
  ScopedHandle ReadEvent(OpenEventA(EVENT_ALL_ACCESS, false, ReadEventName));
  if (ReadEvent == NULL) {
    OutputDebugStringA("OpenEventA failed\n");
    return 1;
  }

  // Collect heaps. This is a loop because GetProcessHeaps requires
  // specifying the max number of heaps to get upfront.
  std::vector<HANDLE> Heaps;
  while (TRUE) {
    DWORD ActualHeapCount = GetProcessHeaps(Heaps.size(), Heaps.data());
    if (ActualHeapCount <= Heaps.size()) {
      Heaps.resize(ActualHeapCount);
      break;
    }
    Heaps.resize(ActualHeapCount);
  }

  // Iterate heaps and heap entries
  size_t Count = 0;
  for (HANDLE Heap : Heaps) {
    PROCESS_HEAP_ENTRY Entry;

    // NOTE: Be careful not to reenter the heap lock while holding the
    // heap lock or else it would hang.
    ScopedHeapLock HeapLock(Heap);
    if (HeapLock.Failure) {
      continue;
    }

    Entry.lpData = NULL;
    while (HeapWalk(Heap, &Entry)) {
      if ((Entry.wFlags & PROCESS_HEAP_REGION) ||
          (Entry.wFlags & PROCESS_HEAP_UNCOMMITTED_RANGE) ||
          (!(Entry.wFlags & PROCESS_HEAP_ENTRY_BUSY))) {
        continue;
      }
      assert(Count < BUF_NUM_ENTRIES);
      Buf.as<HeapEntry>()[Count] = {
        reinterpret_cast<uintptr_t>(Entry.lpData),
        Entry.cbData + Entry.cbOverhead
      };
      if (++Count == BUF_NUM_ENTRIES) {
        if (!SetEvent(ReadEvent)) {
          OutputDebugStringA("SetEvent on ReadEvent failed\n");
          return 1;
        }
        DWORD Wait = WaitForSingleObject(WriteEvent, WAIT_TIMEOUT_MS);
        if (Wait != WAIT_OBJECT_0) {
          char Msg[128];
          if (StringCbPrintfA(Msg, sizeof(Msg),
                              "WaitForSingleObject failed %lu\n",
                              Wait) == S_OK) {
            OutputDebugStringA(Msg);
          }
          return 1;
        }
        std::memset(Buf.get(), 0, BUF_SIZE);
        Count = 0;
      }
    }

    if (Count > 0) {
      // Write the remaining entries.
      if (!SetEvent(ReadEvent)) {
        OutputDebugStringA("SetEvent on ReadEvent failed\n");
        return 1;
      }
      DWORD Wait = WaitForSingleObject(WriteEvent, WAIT_TIMEOUT_MS);
      if (Wait != WAIT_OBJECT_0) {
        char Msg[128];
        if (StringCbPrintfA(Msg, sizeof(Msg),
                            "WaitForSingleObject failed %lu\n", Wait) == S_OK) {
          OutputDebugStringA(Msg);
        }
        return 1;
      }
      std::memset(Buf.get(), 0, BUF_SIZE);
      Count = 0;
    }
  }

  // Indicate the end of iteration with one last write.
  std::memset(Buf.get(), 0, BUF_SIZE);
  Buf.as<HeapEntry>()[0].Address = -1;
  if (!SetEvent(ReadEvent)) {
    OutputDebugStringA("SetEvent at the end of heap iteration failed\n");
    return 1;
  }
  DWORD Wait = WaitForSingleObject(WriteEvent, WAIT_TIMEOUT_MS);
  if (Wait != WAIT_OBJECT_0) {
    char Msg[128];
    if (StringCbPrintfA(Msg, sizeof(Msg), "WaitForSingleObject failed %lu\n",
                        Wait) == S_OK) {
      OutputDebugStringA(Msg);
    }
    return 1;
  }

  return 0;
}

BOOL APIENTRY DllMain(HANDLE hModule, DWORD ul_reason_for_call,
                      LPVOID lpReserved) {
  if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
    heapWalk();
  }
  return TRUE;
}

#endif
