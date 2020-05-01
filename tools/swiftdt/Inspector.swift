import Foundation
import SwiftRemoteMirror

class Inspector {
  let task: task_t
  let symbolicator: CSTypeRef
  let swiftCore: CSTypeRef
  
  init?(pid: pid_t) {
    task = Self.findTask(pid, tryForkCorpse: false)
    if task == 0 {
      return nil
    }
    symbolicator = CSSymbolicatorCreateWithTask(task)
    swiftCore = CSSymbolicatorGetSymbolOwnerWithNameAtTime(
      symbolicator, "libswiftCore.dylib", kCSNow)
    _ = task_start_peeking(task)
  }
  
  deinit {
    task_stop_peeking(task)
    mach_port_deallocate(mach_task_self_, task)
  }

  static func findTask(_ pid: pid_t, tryForkCorpse: Bool) -> task_t {
    var task = task_t()
    var kr = task_for_pid(mach_task_self_, pid, &task)
    if kr != KERN_SUCCESS {
      print("Unable to get task for pid \(pid): \(machErrStr(kr))", to: &Std.err)
      return 0
    }

    if !tryForkCorpse {
      return task
    }
  
    var corpse = task_t()
    kr = task_generate_corpse(task, &corpse)
    if kr == KERN_SUCCESS {
      task_resume(task)
      mach_port_deallocate(mach_task_self_, task)
      return corpse
    } else {
      print("warning: unable to generate corpse for pid \(pid): \(machErrStr(kr))", to: &Std.err)
      return task
    }
  }
  
  func passContext() -> UnsafeMutableRawPointer {
    return Unmanaged.passRetained(self).toOpaque()
  }
  
  func destroyContext() {
    Unmanaged.passUnretained(self).release()
  }
  
  func getAddr(symbolName: String) -> swift_addr_t {
    let symbol = CSSymbolOwnerGetSymbolWithMangledName(swiftCore, symbolName)
    let range = CSSymbolGetRange(symbol)
    return swift_addr_t(range.location)
  }
  
  enum Callbacks {
    static let QueryDataLayout: @convention(c)
      (UnsafeMutableRawPointer?,
       DataLayoutQueryType,
       UnsafeMutableRawPointer?,
       UnsafeMutableRawPointer?) -> CInt
      = QueryDataLayoutFn
    
    static let Free: (@convention(c) (UnsafeMutableRawPointer?,
                                      UnsafeRawPointer?,
                                      UnsafeMutableRawPointer?) -> Void)? = nil
    
    static let ReadBytes: @convention(c)
      (UnsafeMutableRawPointer?,
       swift_addr_t,
       UInt64,
       UnsafeMutablePointer<UnsafeMutableRawPointer?>?) ->
       UnsafeRawPointer?
      = ReadBytesFn
    
    static let GetStringLength: @convention(c)
      (UnsafeMutableRawPointer?,
       swift_addr_t) -> UInt64
      = GetStringLengthFn
    
    static let GetSymbolAddress: @convention(c)
      (UnsafeMutableRawPointer?,
       UnsafePointer<CChar>?,
       UInt64) -> swift_addr_t
      = GetSymbolAddressFn
  }
}

private func instance(_ context: UnsafeMutableRawPointer?) -> Inspector {
  return Unmanaged.fromOpaque(context!).takeUnretainedValue()
}

private func QueryDataLayoutFn(context: UnsafeMutableRawPointer?,
                              type: DataLayoutQueryType,
                              inBuffer: UnsafeMutableRawPointer?,
                              outBuffer: UnsafeMutableRawPointer?) -> CInt {
  switch type {
    case DLQ_GetPointerSize:
      let size = UInt8(MemoryLayout<UnsafeRawPointer>.stride)
      outBuffer!.storeBytes(of: size, toByteOffset: 0, as: UInt8.self)
      return 1
    default:
      return 0
  }
}

private func ReadBytesFn(
  context: UnsafeMutableRawPointer?,
  address: swift_addr_t,
  size: UInt64,
  outContext: UnsafeMutablePointer<UnsafeMutableRawPointer?>?) ->
  UnsafeRawPointer? {
  fatalError()
}

private func GetStringLengthFn(context: UnsafeMutableRawPointer?,
                              address: swift_addr_t) -> UInt64 {
  fatalError()
}

private func GetSymbolAddressFn(context: UnsafeMutableRawPointer?,
                                name: UnsafePointer<CChar>?,
                                length: UInt64) -> swift_addr_t {
  let nameStr: String = name!.withMemoryRebound(to: UInt8.self,
                                                capacity: Int(length), {
    let buffer = UnsafeBufferPointer(start: $0, count: Int(length))
    return String(decoding: buffer, as: UTF8.self)
  })
  return instance(context).getAddr(symbolName: nameStr)
}
