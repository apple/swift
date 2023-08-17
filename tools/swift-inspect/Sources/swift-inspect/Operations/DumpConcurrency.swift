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

#if os(iOS) || os(macOS) || os(tvOS) || os(watchOS)

import ArgumentParser
import SwiftRemoteMirror

struct DumpConcurrency: ParsableCommand {
  static let configuration = CommandConfiguration(
    abstract: "Print information about the target's concurrency runtime.")

  @OptionGroup()
  var options: UniversalOptions

  func run() throws {
    try inspect(options: options) { process in
      let dumper = ConcurrencyDumper(context: process.context,
                                     process: process as! DarwinRemoteProcess)
      dumper.dumpTasks()
      dumper.dumpActors()
      dumper.dumpThreads()
    }
  }
}

fileprivate class ConcurrencyDumper {
  let context: SwiftReflectionContextRef
  let process: DarwinRemoteProcess
  let jobMetadata: swift_reflection_ptr_t?
  let taskMetadata: swift_reflection_ptr_t?

  struct TaskInfo {
    var address: swift_reflection_ptr_t
    var kind: UInt32
    var enqueuePriority: UInt32
    var isChildTask: Bool
    var isFuture: Bool
    var isGroupChildTask: Bool
    var isAsyncLetTask: Bool
    var maxPriority: UInt32
    var isCancelled: Bool
    var isStatusRecordLocked: Bool
    var isEscalated: Bool
    var hasIsRunning: Bool
    var isRunning: Bool
    var isEnqueued: Bool
    var threadPort: UInt32?
    var id: UInt64
    var runJob: swift_reflection_ptr_t
    var allocatorSlabPtr: swift_reflection_ptr_t
    var allocatorTotalSize: Int
    var allocatorTotalChunks: Int
    var childTasks: [swift_reflection_ptr_t]
    var asyncBacktrace: [swift_reflection_ptr_t]
    var parent: swift_reflection_ptr_t?
  }

  struct HeapInfo {
    var tasks: [swift_reflection_ptr_t] = []
    var jobs: [swift_reflection_ptr_t] = []
    var actors: [swift_reflection_ptr_t] = []
  }

  lazy var heapInfo: HeapInfo = gatherHeapInfo()

  lazy var threadCurrentTasks = process.currentTasks.filter{ $0.currentTask != 0 }

  lazy var tasks: [swift_reflection_ptr_t: TaskInfo] = gatherTasks()

  var actors: [swift_reflection_ptr_t] {
    heapInfo.actors
  }

  var metadataIsActorCache: [swift_reflection_ptr_t: Bool] = [:]
  var metadataNameCache: [swift_reflection_ptr_t: String?] = [:]

  init(context: SwiftReflectionContextRef, process: DarwinRemoteProcess) {
    self.context = context
    self.process = process

    func getMetadata(symbolName: String) -> swift_reflection_ptr_t? {
      let addr = process.getAddr(symbolName: symbolName)
      if let ptr = process.read(address: addr, size: MemoryLayout<UInt>.size) {
        return swift_reflection_ptr_t(ptr.load(as: UInt.self))
      }
      return nil
    }
    jobMetadata = getMetadata(symbolName: "_swift_concurrency_debug_jobMetadata")
    taskMetadata = getMetadata(symbolName: "_swift_concurrency_debug_asyncTaskMetadata")
  }

  func gatherHeapInfo() -> HeapInfo {
    var result = HeapInfo()

    process.iterateHeap { (pointer, size) in
      let metadata = swift_reflection_ptr_t(swift_reflection_metadataForObject(context, UInt(pointer)))
      if metadata == jobMetadata {
        result.jobs.append(swift_reflection_ptr_t(pointer))
      } else if metadata == taskMetadata {
        result.tasks.append(swift_reflection_ptr_t(pointer))
      } else if isActorMetadata(metadata) {
        result.actors.append(swift_reflection_ptr_t(pointer))
      }
    }

    return result
  }

  func gatherTasks() -> [swift_reflection_ptr_t: TaskInfo] {
    var map: [swift_reflection_ptr_t: TaskInfo] = [:]
    var tasksToScan: Set<swift_reflection_ptr_t> = []
    tasksToScan.formUnion(heapInfo.tasks)
    tasksToScan.formUnion(threadCurrentTasks.map{ swift_reflection_ptr_t($0.currentTask) }.filter{ $0 != 0 })

    while !tasksToScan.isEmpty {
      let taskToScan = tasksToScan.removeFirst()
      if let info = info(forTask: taskToScan) {
        map[taskToScan] = info
        for child in info.childTasks {
          let childMetadata = swift_reflection_metadataForObject(context, UInt(child))
          if let taskMetadata = taskMetadata, childMetadata != taskMetadata {
            print("Inconsistent data detected! Child task \(hex: child) has unknown metadata \(hex: taskMetadata)")
          }
          if map[child] == nil {
            tasksToScan.insert(child)
          }
        }
      }
    }

    for (task, info) in map {
      for child in info.childTasks {
        map[child]?.parent = task
      }
    }

    return map
  }

  func isActorMetadata(_ metadata: swift_reflection_ptr_t) -> Bool {
    if let cached = metadataIsActorCache[metadata] {
      return cached
    }
    let result = swift_reflection_metadataIsActor(context, metadata) != 0
    metadataIsActorCache[metadata] = result
    return result
  }

  func name(metadata: swift_reflection_ptr_t) -> String? {
    if let cached = metadataNameCache[metadata] {
      return cached
    }

    let name = context.name(type: metadata)
    metadataNameCache[metadata] = name
    return name
  }

  func info(forTask task: swift_reflection_ptr_t) -> TaskInfo? {
    let reflectionInfo = swift_reflection_asyncTaskInfo(context, task)
    if let error = reflectionInfo.Error {
      print("Error getting info for async task \(hex: task): \(String(cString: error))")
      return nil
    }

    // These arrays are temporary pointers which we must copy out before we call
    // into Remote Mirror again.
    let children = Array(UnsafeBufferPointer(
        start: reflectionInfo.ChildTasks,
        count: Int(reflectionInfo.ChildTaskCount)))
    let asyncBacktraceFrames = Array(UnsafeBufferPointer(
        start: reflectionInfo.AsyncBacktraceFrames,
        count: Int(reflectionInfo.AsyncBacktraceFramesCount)))

    var allocatorSlab = reflectionInfo.AllocatorSlabPtr
    var allocatorTotalSize = 0
    var allocatorTotalChunks = 0
    while allocatorSlab != 0 {
      let allocations = swift_reflection_asyncTaskSlabAllocations(context,
                                                                  allocatorSlab)
      guard allocations.Error == nil else { break }
      allocatorTotalSize += Int(allocations.SlabSize)
      allocatorTotalChunks += Int(allocations.ChunkCount)

      allocatorSlab = allocations.NextSlab
    }

    return TaskInfo(
      address: task,
      kind: reflectionInfo.Kind,
      enqueuePriority: reflectionInfo.EnqueuePriority,
      isChildTask: reflectionInfo.IsChildTask,
      isFuture: reflectionInfo.IsFuture,
      isGroupChildTask: reflectionInfo.IsGroupChildTask,
      isAsyncLetTask: reflectionInfo.IsAsyncLetTask,
      maxPriority: reflectionInfo.MaxPriority,
      isCancelled: reflectionInfo.IsCancelled,
      isStatusRecordLocked: reflectionInfo.IsStatusRecordLocked,
      isEscalated: reflectionInfo.IsEscalated,
      hasIsRunning: reflectionInfo.HasIsRunning,
      isRunning: reflectionInfo.IsRunning,
      isEnqueued: reflectionInfo.IsEnqueued,
      threadPort: reflectionInfo.HasThreadPort
                ? reflectionInfo.ThreadPort
                : nil,
      id: reflectionInfo.Id,
      runJob: reflectionInfo.RunJob,
      allocatorSlabPtr: reflectionInfo.AllocatorSlabPtr,
      allocatorTotalSize: allocatorTotalSize,
      allocatorTotalChunks: allocatorTotalChunks,
      childTasks: children,
      asyncBacktrace: asyncBacktraceFrames
    )
  }

  func taskHierarchy() -> [(level: Int, lastChild: Bool, task: TaskInfo)] {
    var hierarchy: [(level: Int, lastChild: Bool, task: TaskInfo)] = []

    let topLevelTasks = tasks.values.filter{ $0.parent == nil }
    for top in topLevelTasks.sorted(by: { $0.id < $1.id }) {
      var stack: [(index: Int, task: TaskInfo)] = [(0, top)]
      hierarchy.append((0, true, top))

      while let (index, task) = stack.popLast() {
        if index < task.childTasks.count {
          stack.append((index + 1, task))
          let childPtr = task.childTasks[index]
          let childTask = tasks[childPtr]!
          hierarchy.append((stack.count, index == task.childTasks.count - 1, childTask))
          stack.append((0, childTask))
        }
      }
    }
    return hierarchy
  }

  func remove(from: String, upTo: String) -> String {
    from.withCString {
      if let found = strstr($0, upTo) {
        return String(cString: found + strlen(upTo))
      }
      return from
    }
  }

  func symbolicateBacktracePointer(ptr: swift_reflection_ptr_t) -> String {
    guard let name = process.symbolicate(swift_addr_t(ptr)).symbol else {
      return "<\(hex: ptr)>"
    }

    return remove(from: name, upTo: " resume partial function for ")
  }

  func decodeTaskFlags(_ info: TaskInfo) -> String {
    var flags: [String] = []
    if info.isChildTask { flags.append("childTask") }
    if info.isFuture { flags.append("future") }
    if info.isGroupChildTask { flags.append("groupChildTask") }
    if info.isAsyncLetTask { flags.append("asyncLetTask") }
    if info.isCancelled { flags.append("cancelled") }
    if info.isStatusRecordLocked { flags.append("statusRecordLocked") }
    if info.isEscalated { flags.append("escalated") }
    if info.hasIsRunning && info.isRunning { flags.append("running") }
    if info.isEnqueued { flags.append("enqueued") }

    let flagsStr = flags.isEmpty ? "0" : flags.joined(separator: "|")
    return flagsStr
  }

  func decodeActorFlags(_ info: swift_actor_info_t) -> (
    state: String,
    flags: String,
    maxPriority: UInt8
  ) {
    let states: [UInt8: String] = [
      0: "idle",
      1: "scheduled",
      2: "running",
      3: "zombie-latching",
      4: "zombie-ready-for-deallocation"
    ]

    var flags: [String] = []
    if info.IsPriorityEscalated { flags.append("priorityEscalated") }
    if info.IsDistributedRemote { flags.append("distributedRemote") }
    let flagsStr = flags.isEmpty ? "0" : flags.joined(separator: "|")

    return (
      state: states[info.State] ?? "unknown(\(info.State))",
      flags: flagsStr,
      maxPriority: info.MaxPriority
    )
  }

  func dumpTasks() {
    print("TASKS")

    let missingIsRunning = tasks.contains(where: { !$1.hasIsRunning })
    if missingIsRunning {
      print("warning: unable to decode is-running state of target tasks, running state and async backtraces will not be printed")
    }

    let taskToThread: [swift_addr_t: UInt64] =
        Dictionary(threadCurrentTasks.map{ ($1, $0) }, uniquingKeysWith: { $1 })

    var lastChilds: [Bool] = []

    let hierarchy = taskHierarchy()
    for (i, (level, lastChild, task)) in hierarchy.enumerated() {
      lastChilds.removeSubrange(level...)
      lastChilds.append(lastChild)

      let prevEntry = i > 0 ? hierarchy[i - 1] : nil

      let levelDidIncrease = level > (prevEntry?.level ?? -1)

      var prefix = ""
      for lastChild in lastChilds {
        prefix += lastChild ? "     " : "   | "
      }
      prefix += "   "
      let firstPrefix = String(prefix.dropLast(5) + (
          level == 0 ? "   " :
          lastChild  ? "`--" :
                       "+--"))
      if levelDidIncrease {
        print(prefix)
      }

      var firstLine = true
      func output(_ str: String) {
        print((firstLine ? firstPrefix : prefix) + str)
        firstLine = false
      }

      let runJobSymbol = process.symbolicate(swift_addr_t(task.runJob))
      let runJobLibrary = runJobSymbol.module ?? "<unknown>"

      let symbolicatedBacktrace = task.asyncBacktrace.map(symbolicateBacktracePointer)

      let flags = decodeTaskFlags(task)

      output("Task \(hex: task.id) - flags=\(flags) enqueuePriority=\(hex: task.enqueuePriority) maxPriority=\(hex: task.maxPriority) address=\(hex: task.address)")
      if let thread = taskToThread[swift_addr_t(task.address)] {
        output("current task on thread \(hex: thread)")
      }
      if let parent = task.parent {
        output("parent: \(hex: parent)")
      }
      if let threadPort = task.threadPort, threadPort != 0 {
        if let threadID = process.getThreadID(remotePort: threadPort) {
          output("waiting on thread: port=\(hex: threadPort) id=\(hex: threadID)")
        }
      }

      if let first = symbolicatedBacktrace.first {
        output("async backtrace: \(first)")
        for entry in symbolicatedBacktrace.dropFirst() {
          output("                 \(entry)")
        }
      }

      output("resume function: \(symbolicateBacktracePointer(ptr: task.runJob)) in \(runJobLibrary)")
      output("task allocator: \(task.allocatorTotalSize) bytes in \(task.allocatorTotalChunks) chunks")

      if task.childTasks.count > 0 {
        let s = task.childTasks.count > 1 ? "s" : ""
        output("* \(task.childTasks.count) child task\(s)")
      }

      if (task.childTasks.isEmpty) && i < hierarchy.count - 1 {
        print(prefix)
      }
    }

    print("")
  }

  func dumpActors() {
    print("ACTORS")

    for actor in actors {
      let metadata = swift_reflection_metadataForObject(context, UInt(actor))
      let metadataName = name(metadata: swift_reflection_ptr_t(metadata)) ?? "<unknown class name>"
      let info = swift_reflection_actorInfo(context, actor)
      if let error = info.Error {
        print(String(utf8String: error) ?? "<unknown error>")
        continue
      }

      let flags = decodeActorFlags(info)

      print("  \(hex: actor) \(metadataName) state=\(flags.state) flags=\(flags.flags) maxPriority=\(hex: flags.maxPriority)")
      if info.HasThreadPort && info.ThreadPort != 0 {
        if let threadID = process.getThreadID(remotePort: info.ThreadPort) {
          print("    waiting on thread: port=\(hex: info.ThreadPort) id=\(hex: threadID)")
        } else {
          print("    waiting on thread: port=\(hex: info.ThreadPort) (unknown thread ID)")
        }
      }

      func jobStr(_ job: swift_reflection_ptr_t) -> String {
        if let task = tasks[job] {
          return "Task \(hex: task.id) \(symbolicateBacktracePointer(ptr: task.runJob))"
        }
        return "<internal job \(hex: job)>"
      }

      var job = info.FirstJob
      if job == 0 {
        print("    no jobs queued")
      } else {
        print("    job queue: \(jobStr(job))")
        let limit = 1000
        for i in 1 ... limit {
          job = swift_reflection_nextJob(context, job);
          if job != 0 {
            print("               \(jobStr(job))")
          } else {
            break
          }

          if i == limit {
            print("               ...truncated...")
          }
        }
      }
      print("")
    }
  }

  func dumpThreads() {
    print("THREADS")
    if threadCurrentTasks.isEmpty {
      print("  no threads with active tasks")
      return
    }

    for (thread, task) in threadCurrentTasks {
      let taskStr: String
      if let info = tasks[swift_reflection_ptr_t(task)] {
        taskStr = "\(hex: info.id)"
      } else {
        taskStr = "<unknown task \(hex: task)>"
      }
      print("  Thread \(hex: thread) - current task: \(taskStr)")
    }
  }
}

#endif
