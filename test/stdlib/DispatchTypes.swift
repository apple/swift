// RUN: %target-swift-frontend -typecheck %s

// REQUIRES: libdispatch
// UNSUPPORTED: OS=linux-gnu
// UNSUPPORTED: OS=linux-android
// UNSUPPORTED: OS=openbsd
// UNSUPPORTED: OS=freebsd

import Dispatch

func getAnyValue<T>(_ opt: T?) -> T { return opt! }

// dispatch/io.h
let io = DispatchIO(type: .stream, fileDescriptor: 0, queue: DispatchQueue.main, cleanupHandler: { (error: Int32) -> () in fatalError() })
io.close(flags: .stop)
io.setInterval(interval: .seconds(0), flags: .strictInterval)

// dispatch/queue.h
let q = DispatchQueue(label: "", attributes: [])
_ = DispatchQueue(label: "", attributes: .concurrent)
_ = q.label
if #available(OSX 10.10, iOS 8.0, *) {
	_ = DispatchQueue.global(qos: .userInteractive)
	_ = DispatchQueue.global(qos: .background)
	_ = DispatchQueue.global(qos: .default)
}

// dispatch/source.h
_ = DispatchSource.makeUserDataAddSource()
_ = DispatchSource.makeUserDataOrSource()
_ = DispatchSource.makeMachSendSource(port: mach_port_t(0), eventMask: [])
_ = DispatchSource.makeMachReceiveSource(port: mach_port_t(0))
_ = DispatchSource.makeMemoryPressureSource(eventMask: [])
_ = DispatchSource.makeProcessSource(identifier: 0, eventMask: [])
_ = DispatchSource.makeReadSource(fileDescriptor: 0)
_ = DispatchSource.makeSignalSource(signal: SIGINT)
_ = DispatchSource.makeTimerSource()
_ = DispatchSource.makeFileSystemObjectSource(fileDescriptor: 0, eventMask: [])
_ = DispatchSource.makeWriteSource(fileDescriptor: 0)

// dispatch/time.h
_ = DispatchTime.now()
_ = DispatchTime.distantFuture
