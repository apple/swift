//===----------------------------------------------------------------------===//
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

// dispatch/queue.h

@_implementationOnly import _SwiftDispatchOverlayShims

public final class DispatchSpecificKey<T> {
	public init() {}
}

internal class _DispatchSpecificValue<T> {
	internal let value: T
	internal init(value: T) { self.value = value }
}

extension DispatchQueue {
	public struct Attributes : OptionSet {
		public let rawValue: UInt64
		public init(rawValue: UInt64) { self.rawValue = rawValue }

		public static let concurrent = Attributes(rawValue: 1<<1)

		@available(macOS 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)
		public static let initiallyInactive = Attributes(rawValue: 1<<2)

		fileprivate func _attr() -> __OS_dispatch_queue_attr? {
			var attr: __OS_dispatch_queue_attr?

			if self.contains(.concurrent) {
				attr = _swift_dispatch_queue_concurrent()
			}
			if #available(macOS 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *) {
				if self.contains(.initiallyInactive) {
					attr = __dispatch_queue_attr_make_initially_inactive(attr)
				}
			}
			return attr
		}
	}

	public enum GlobalQueuePriority {
		@available(macOS, deprecated: 10.10, message: "Use qos attributes instead")
		@available(iOS, deprecated: 8.0, message: "Use qos attributes instead")
		@available(tvOS, deprecated, message: "Use qos attributes instead")
		@available(watchOS, deprecated, message: "Use qos attributes instead")
		case high

		@available(macOS, deprecated: 10.10, message: "Use qos attributes instead")
		@available(iOS, deprecated: 8.0, message: "Use qos attributes instead")
		@available(tvOS, deprecated, message: "Use qos attributes instead")
		@available(watchOS, deprecated, message: "Use qos attributes instead")
		case `default`

		@available(macOS, deprecated: 10.10, message: "Use qos attributes instead")
		@available(iOS, deprecated: 8.0, message: "Use qos attributes instead")
		@available(tvOS, deprecated, message: "Use qos attributes instead")
		@available(watchOS, deprecated, message: "Use qos attributes instead")
		case low

		@available(macOS, deprecated: 10.10, message: "Use qos attributes instead")
		@available(iOS, deprecated: 8.0, message: "Use qos attributes instead")
		@available(tvOS, deprecated, message: "Use qos attributes instead")
		@available(watchOS, deprecated, message: "Use qos attributes instead")
		case background

		internal var _translatedValue: Int {
			switch self {
			case .high: return 2 // DISPATCH_QUEUE_PRIORITY_HIGH
			case .default: return 0 // DISPATCH_QUEUE_PRIORITY_DEFAULT
			case .low: return -2 // DISPATCH_QUEUE_PRIORITY_LOW
			case .background: return Int(Int16.min) // DISPATCH_QUEUE_PRIORITY_BACKGROUND
			}
		}
	}

	public enum AutoreleaseFrequency {
		case inherit

		@available(macOS 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)
		case workItem

		@available(macOS 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)
		case never

		internal func _attr(attr: __OS_dispatch_queue_attr?) -> __OS_dispatch_queue_attr? {
			if #available(macOS 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *) {
				return __dispatch_queue_attr_make_with_autorelease_frequency(attr, self._rawValue)
			} else {
				return attr
			}
		}

		internal var _rawValue: __dispatch_autorelease_frequency_t {
			switch self {
			case .inherit:
				// DISPATCH_AUTORELEASE_FREQUENCY_INHERIT
				return (__dispatch_autorelease_frequency_t(rawValue: 0) as Optional)!
			case .workItem:
				// DISPATCH_AUTORELEASE_FREQUENCY_WORK_ITEM
				return (__dispatch_autorelease_frequency_t(rawValue: 1) as Optional)!
			case .never:
				// DISPATCH_AUTORELEASE_FREQUENCY_NEVER
				return (__dispatch_autorelease_frequency_t(rawValue: 2) as Optional)!
			}
		}
	}

	public class func concurrentPerform(iterations: Int, execute work: (Int) -> Void) {
		_swift_dispatch_apply_current(iterations, work)
	}

	public class var main: DispatchQueue {
		return _swift_dispatch_get_main_queue()
	}

	@available(macOS, deprecated: 10.10)
	@available(iOS, deprecated: 8.0)
	@available(tvOS, deprecated)
	@available(watchOS, deprecated)
	public class func global(priority: GlobalQueuePriority) -> DispatchQueue {
		return __dispatch_get_global_queue(priority._translatedValue, 0)
	}

	@available(macOS 10.10, iOS 8.0, *)
	public class func global(qos: DispatchQoS.QoSClass = .default) -> DispatchQueue {
		return __dispatch_get_global_queue(Int(qos.rawValue.rawValue), 0)
	}

	public class func getSpecific<T>(key: DispatchSpecificKey<T>) -> T? {
		let k = Unmanaged.passUnretained(key).toOpaque()
		if let p = __dispatch_get_specific(k) {
			let v = Unmanaged<_DispatchSpecificValue<T>>
				.fromOpaque(p)
				.takeUnretainedValue()
			return v.value
		}
		return nil
	}

	public convenience init(
		label: String,
		qos: DispatchQoS = .unspecified,
		attributes: Attributes = [],
		autoreleaseFrequency: AutoreleaseFrequency = .inherit,
		target: DispatchQueue? = nil)
	{
		var attr = attributes._attr()
		if autoreleaseFrequency != .inherit {
			attr = autoreleaseFrequency._attr(attr: attr)
		}
		if #available(macOS 10.10, iOS 8.0, *), qos != .unspecified {
			attr = __dispatch_queue_attr_make_with_qos_class(attr, qos.qosClass.rawValue, Int32(qos.relativePriority))
		}

		if #available(macOS 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *) {
			self.init(__label: label, attr: attr, queue: target)
		} else {
			self.init(__label: label, attr: attr)
			if let tq = target { self.setTarget(queue: tq) }
		}
	}

	public var label: String {
		return String(validatingUTF8: __dispatch_queue_get_label(self))!
	}

	///
	/// Submits a block for synchronous execution on this queue.
	///
	/// Submits a work item to a dispatch queue like `async(execute:)`, however
	/// `sync(execute:)` will not return until the work item has finished.
	///
	/// Work items submitted to a queue with `sync(execute:)` do not observe certain
	/// queue attributes of that queue when invoked (such as autorelease frequency
	/// and QoS class).
	///
	/// Calls to `sync(execute:)` targeting the current queue will result
	/// in deadlock. Use of `sync(execute:)` is also subject to the same
	/// multi-party deadlock problems that may result from the use of a mutex.
	/// Use of `async(execute:)` is preferred.
	///
	/// As an optimization, `sync(execute:)` invokes the work item on the thread which
	/// submitted it, except when the queue is the main queue or
	/// a queue targetting it.
	///
	/// - parameter execute: The work item to be invoked on the queue.
	/// - SeeAlso: `async(execute:)`
	///
	@available(macOS 10.10, iOS 8.0, *)
	public func sync(execute workItem: DispatchWorkItem) {
		// _swift_dispatch_sync preserves the @convention(block) for
		// work item blocks.
		_swift_dispatch_sync(self, workItem._block)
	}

	///
	/// Submits a work item for asynchronous execution on a dispatch queue.
	///
	/// `async(execute:)` is the fundamental mechanism for submitting
	/// work items to a dispatch queue.
	///
	/// Calls to `async(execute:)` always return immediately after the work item has
	/// been submitted, and never wait for the work item to be invoked.
	///
	/// The target queue determines whether the work item will be invoked serially or
	/// concurrently with respect to other work items submitted to that same queue.
	/// Serial queues are processed concurrently with respect to each other.
	///
	/// - parameter execute: The work item to be invoked on the queue.
	/// - SeeAlso: `sync(execute:)`
	///
	///
	@available(macOS 10.10, iOS 8.0, *)
	public func async(execute workItem: DispatchWorkItem) {
		// _swift_dispatch_async preserves the @convention(block)
		// for work item blocks.
		_swift_dispatch_async(self, workItem._block)
	}

	///
	/// Submits a work item to a dispatch queue and associates it with the given
	/// dispatch group. The dispatch group may be used to wait for the completion
	/// of the work items it references.
	///
	/// - parameter group: the dispatch group to associate with the submitted block.
	/// - parameter execute: The work item to be invoked on the queue.
	/// - SeeAlso: `sync(execute:)`
	///
	@available(macOS 10.10, iOS 8.0, *)
	public func async(group: DispatchGroup, execute workItem: DispatchWorkItem) {
		// _swift_dispatch_group_async preserves the @convention(block)
		// for work item blocks.
		_swift_dispatch_group_async(group, self, workItem._block)
	}

	///
	/// Submits a work item to a dispatch queue and optionally associates it with a
	/// dispatch group. The dispatch group may be used to wait for the completion
	/// of the work items it references.
	///
	/// - parameter group: the dispatch group to associate with the submitted
	/// work item. If this is `nil`, the work item is not associated with a group.
	/// - parameter flags: flags that control the execution environment of the
	/// - parameter qos: the QoS at which the work item should be executed.
	///	Defaults to `DispatchQoS.unspecified`.
	/// - parameter flags: flags that control the execution environment of the
	/// work item.
	/// - parameter execute: The work item to be invoked on the queue.
	/// - SeeAlso: `sync(execute:)`
	/// - SeeAlso: `DispatchQoS`
	/// - SeeAlso: `DispatchWorkItemFlags`
	///
	public func async(
		group: DispatchGroup? = nil,
		qos: DispatchQoS = .unspecified,
		flags: DispatchWorkItemFlags = [],
		execute work: @escaping @convention(block) () -> Void)
	{
		if group == nil && qos == .unspecified {
			// Fast-path route for the most common API usage
			if flags.isEmpty {
				_swift_dispatch_async(self, work)
				return
			} else if flags == .barrier {
				_swift_dispatch_barrier_async(self, work)
				return
			}
		}

		var block: @convention(block) () -> Void = work
		if #available(macOS 10.10, iOS 8.0, *), (qos != .unspecified || !flags.isEmpty) {
			let workItem = DispatchWorkItem(qos: qos, flags: flags, block: work)
			block = workItem._block
		}

		if let g = group {
			_swift_dispatch_group_async(g, self, block)
		} else {
			_swift_dispatch_async(self, block)
		}
	}

	private func _syncBarrier(block: () -> Void) {
		__dispatch_barrier_sync(self, block)
	}

	@_rethrowsUnchecked
	private func _syncHelper<T>(
		fn: (() -> Void) -> Void,
		execute work: () throws -> T) rethrows -> T
	{
		var result: Result<T, Error>!
		withoutActuallyEscaping(work) { _work in
			fn {
				result = Result(catching: _work)
			}
		}
		return try result.get()
	}

	@_rethrowsUnchecked
	@available(macOS 10.10, iOS 8.0, *)
	private func _syncHelper<T>(
		fn: (DispatchWorkItem) -> Void,
		flags: DispatchWorkItemFlags,
		execute work: () throws -> T) rethrows -> T
	{
		var result: Result<T, Error>!
		let workItem = DispatchWorkItem(flags: flags, noescapeBlock: {
			result = Result(catching: work)
		})
		fn(workItem)
		return try result.get()
	}

	///
	/// Submits a block for synchronous execution on this queue.
	///
	/// Submits a work item to a dispatch queue like `sync(execute:)`, and returns
	/// the value, of type `T`, returned by that work item.
	///
	/// - parameter execute: The work item to be invoked on the queue.
	/// - returns the value returned by the work item.
	/// - SeeAlso: `sync(execute:)`
	///
	public func sync<T>(execute work: () throws -> T) rethrows -> T {
		return try self._syncHelper(fn: sync, execute: work)
	}

	///
	/// Submits a block for synchronous execution on this queue.
	///
	/// Submits a work item to a dispatch queue like `sync(execute:)`, and returns
	/// the value, of type `T`, returned by that work item.
	///
	/// - parameter flags: flags that control the execution environment of the
	/// - parameter execute: The work item to be invoked on the queue.
	/// - returns the value returned by the work item.
	/// - SeeAlso: `sync(execute:)`
	/// - SeeAlso: `DispatchWorkItemFlags`
	///
	public func sync<T>(flags: DispatchWorkItemFlags, execute work: () throws -> T) rethrows -> T {
		if flags == .barrier {
			return try self._syncHelper(fn: _syncBarrier, execute: work)
		} else if #available(macOS 10.10, iOS 8.0, *), !flags.isEmpty {
			return try self._syncHelper(fn: sync, flags: flags, execute: work)
		} else {
			return try self._syncHelper(fn: sync, execute: work)
		}
	}

	///
	/// Submits a work item to a dispatch queue for asynchronous execution after
	/// a specified time.
	///
	/// - parameter: deadline the time after which the work item should be executed,
	/// given as a `DispatchTime`.
	/// - parameter qos: the QoS at which the work item should be executed.
	///	Defaults to `DispatchQoS.unspecified`.
	/// - parameter flags: flags that control the execution environment of the
	/// work item.
	/// - parameter execute: The work item to be invoked on the queue.
	/// - SeeAlso: `async(execute:)`
	/// - SeeAlso: `asyncAfter(deadline:execute:)`
	/// - SeeAlso: `DispatchQoS`
	/// - SeeAlso: `DispatchWorkItemFlags`
	/// - SeeAlso: `DispatchTime`
	///
	public func asyncAfter(
		deadline: DispatchTime,
		qos: DispatchQoS = .unspecified,
		flags: DispatchWorkItemFlags = [],
		execute work: @escaping @convention(block) () -> Void)
	{
		if #available(macOS 10.10, iOS 8.0, *), qos != .unspecified || !flags.isEmpty {
			let item = DispatchWorkItem(qos: qos, flags: flags, block: work)
			_swift_dispatch_after(deadline.rawValue, self, item._block)
		} else {
			_swift_dispatch_after(deadline.rawValue, self, work)
		}
	}

	///
	/// Submits a work item to a dispatch queue for asynchronous execution after
	/// a specified time.
	///
	/// - parameter: deadline the time after which the work item should be executed,
	/// given as a `DispatchWallTime`.
	/// - parameter qos: the QoS at which the work item should be executed.
	///	Defaults to `DispatchQoS.unspecified`.
	/// - parameter flags: flags that control the execution environment of the
	/// work item.
	/// - parameter execute: The work item to be invoked on the queue.
	/// - SeeAlso: `async(execute:)`
	/// - SeeAlso: `asyncAfter(wallDeadline:execute:)`
	/// - SeeAlso: `DispatchQoS`
	/// - SeeAlso: `DispatchWorkItemFlags`
	/// - SeeAlso: `DispatchWallTime`
	///
	public func asyncAfter(
		wallDeadline: DispatchWallTime,
		qos: DispatchQoS = .unspecified,
		flags: DispatchWorkItemFlags = [],
		execute work: @escaping @convention(block) () -> Void)
	{
		if #available(macOS 10.10, iOS 8.0, *), qos != .unspecified || !flags.isEmpty {
			let item = DispatchWorkItem(qos: qos, flags: flags, block: work)
			_swift_dispatch_after(wallDeadline.rawValue, self, item._block)
		} else {
			_swift_dispatch_after(wallDeadline.rawValue, self, work)
		}
	}

	///
	/// Submits a work item to a dispatch queue for asynchronous execution after
	/// a specified time.
	///
	/// - parameter: deadline the time after which the work item should be executed,
	/// given as a `DispatchTime`.
	/// - parameter execute: The work item to be invoked on the queue.
	/// - SeeAlso: `asyncAfter(deadline:qos:flags:execute:)`
	/// - SeeAlso: `DispatchTime`
	///
	@available(macOS 10.10, iOS 8.0, *)
	public func asyncAfter(deadline: DispatchTime, execute: DispatchWorkItem) {
		_swift_dispatch_after(deadline.rawValue, self, execute._block)
	}

	///
	/// Submits a work item to a dispatch queue for asynchronous execution after
	/// a specified time.
	///
	/// - parameter: deadline the time after which the work item should be executed,
	/// given as a `DispatchWallTime`.
	/// - parameter execute: The work item to be invoked on the queue.
	/// - SeeAlso: `asyncAfter(wallDeadline:qos:flags:execute:)`
	/// - SeeAlso: `DispatchTime`
	///
	@available(macOS 10.10, iOS 8.0, *)
	public func asyncAfter(wallDeadline: DispatchWallTime, execute: DispatchWorkItem) {
		_swift_dispatch_after(wallDeadline.rawValue, self, execute._block)
	}

	@available(macOS 10.10, iOS 8.0, *)
	public var qos: DispatchQoS {
		var relPri: Int32 = 0
		let cls = DispatchQoS.QoSClass(rawValue: __dispatch_queue_get_qos_class(self, &relPri))!
		return DispatchQoS(qosClass: cls, relativePriority: Int(relPri))
	}

	public func getSpecific<T>(key: DispatchSpecificKey<T>) -> T? {
		let k = Unmanaged.passUnretained(key).toOpaque()
		if let p = __dispatch_queue_get_specific(self, k) {
			let v = Unmanaged<_DispatchSpecificValue<T>>
				.fromOpaque(p)
				.takeUnretainedValue()
			return v.value
		}
		return nil
	}

	public func setSpecific<T>(key: DispatchSpecificKey<T>, value: T?) {
		let k = Unmanaged.passUnretained(key).toOpaque()
		let v = value.map { _DispatchSpecificValue(value: $0) }
		let p = v.map { Unmanaged.passRetained($0).toOpaque() }
		__dispatch_queue_set_specific(self, k, p, _destructDispatchSpecificValue)
	}
}

private func _destructDispatchSpecificValue(ptr: UnsafeMutableRawPointer?) {
	if let p = ptr {
		Unmanaged<AnyObject>.fromOpaque(p).release()
	}
}
