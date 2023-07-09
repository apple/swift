@inline(never)
private func consume<T>(_ t: T) {
    withExtendedLifetime(t) { t in }
}
@usableFromInline
internal func _prespecialize() {
  consume(Array<()>.self)
  consume(Array<(Optional<String>, Any)>.self)
  consume(Array<Any>.self)
  consume(Array<AnyHashable>.self)
  consume(Array<Dictionary<String, Any>>.self)
  consume(Array<Dictionary<String, AnyObject>>.self)
  consume(Array<Int64>.self)
  consume(Array<Int>.self)
  consume(Array<Optional<Any>>.self)
  consume(Array<Optional<String>>.self)
#if _runtime(_ObjC)
  consume(_ArrayBuffer<()>.self)
  consume(_ArrayBuffer<(Optional<String>, Any)>.self)
  consume(_ArrayBuffer<Any>.self)
  consume(_ArrayBuffer<AnyHashable>.self)
  consume(_ArrayBuffer<Dictionary<String, Any>>.self)
  consume(_ArrayBuffer<Dictionary<String, AnyObject>>.self)
  consume(_ArrayBuffer<Int64>.self)
  consume(_ArrayBuffer<Int>.self)
  consume(_ArrayBuffer<Optional<Any>>.self)
  consume(_ArrayBuffer<Optional<String>>.self)
#endif
  consume(ClosedRange<Int>.self)
  consume(ContiguousArray<(AnyHashable, Any)>.self)
  consume(ContiguousArray<(Optional<String>, Any)>.self)
  consume(ContiguousArray<Any>.self)
  consume(ContiguousArray<AnyHashable>.self)
  consume(ContiguousArray<Dictionary<String, AnyObject>>.self)
  consume(ContiguousArray<Int64>.self)
  consume(ContiguousArray<String>.self)
  consume(_ContiguousArrayStorage<(AnyHashable, Any)>.self)
  consume(_ContiguousArrayStorage<(Optional<String>, Any)>.self)
  consume(_ContiguousArrayStorage<Any>.self)
  consume(_ContiguousArrayStorage<AnyHashable>.self)
  consume(_ContiguousArrayStorage<Dictionary<String, AnyObject>>.self)
  consume(_ContiguousArrayStorage<Int64>.self)
  consume(_ContiguousArrayStorage<String>.self)
  consume(Dictionary<String, String>.Index.self)
  consume(Dictionary<String, String>.Iterator.self)
  consume(Dictionary<AnyHashable, Any>.self)
  consume(Dictionary<AnyHashable, Any>.Iterator.self)
  consume(Dictionary<AnyHashable, AnyHashable>.self)
  consume(Dictionary<AnyHashable, AnyHashable>.Iterator.self)
  consume(Dictionary<AnyHashable, AnyObject>.self)
  consume(Dictionary<String, Any>.self)
  consume(Dictionary<String, Any>.Iterator.self)
  consume(Dictionary<String, AnyHashable>.self)
  consume(Dictionary<String, AnyObject>.self)
  consume(Dictionary<String, Array<String>>.self)
  consume(Dictionary<String, Dictionary<String, Any>>.self)
  consume(Dictionary<String, String>.self)
  consume(Dictionary<String, UInt8>.self)
  consume(IndexingIterator<Array<Dictionary<String, AnyObject>>>.self)
  consume(IndexingIterator<Array<Int>>.self)
  consume(IndexingIterator<Array<String>>.self)
  consume(IndexingIterator<ClosedRange<Int>>.self)
  consume(IndexingIterator<Range<Int>>.self)
  consume(IndexingIterator<String.UTF8View>.self)
  consume(Optional<()>.self)
  consume(Optional<(Any, Any)>.self)
  consume(Optional<(AnyHashable, Any)>.self)
  consume(Optional<(AnyHashable, AnyHashable)>.self)
  consume(Optional<(CodingUserInfoKey, Any)>.self)
  consume(Optional<(Int64, Bool)>.self)
  consume(Optional<(Optional<String>, Any)>.self)
  consume(Optional<(String, Any)>.self)
  consume(Optional<(String, AnyObject)>.self)
  consume(Optional<(String, Array<String>)>.self)
  consume(Optional<(String, Int64)>.self)
  consume(Optional<(String, String)>.self)
  consume(Optional<(String, UInt8)>.self)
  consume(Optional<Any>.self)
  consume(Optional<AnyObject>.self)
  consume(Optional<Array<Int64>>.self)
  consume(Optional<Array<String>>.self)
  consume(Optional<Bool>.self)
  consume(Optional<CodingKey>.self)
  consume(Optional<CodingUserInfoKey>.self)
  consume(Optional<CustomDebugStringConvertible>.self)
#if SWIFT_ENABLE_REFLECTION
  consume(Optional<CustomReflectable>.self)
#endif
  consume(Optional<CustomStringConvertible>.self)
  consume(Optional<Dictionary<AnyHashable, Any>>.self)
  consume(Optional<Dictionary<String, Any>>.self)
  consume(Optional<Dictionary<String, AnyObject>>.self)
  consume(Optional<Dictionary<String, Array<String>>>.self)
  consume(Optional<Dictionary<String, String>>.self)
  consume(Optional<Double>.self)
  consume(Optional<Int64>.self)
  consume(Optional<Int8>.self)
#if SWIFT_ENABLE_REFLECTION
  consume(Optional<Mirror.DisplayStyle>.self)
#endif
  consume(Optional<Optional<Int64>>.self)
  consume(Optional<Optional<String>>.self)
  consume(Optional<Set<String>>.self)
  consume(Optional<TextOutputStreamable>.self)
  consume(Optional<UInt8>.self)
  consume(Optional<UnsafeBufferPointer<AnyObject>>.self)
  consume(PartialRangeFrom<Int>.self)
  consume(Range<String.Index>.self)
  consume(ReversedCollection<Range<Int>>.self)
  consume(ReversedCollection<Range<Int>>.Iterator.self)
  consume(Set<Int>.self)
  consume(Set<String>.self)
  consume(Set<String>.Iterator.self)
  consume(Set<String>.self)
  consume(Unmanaged<AnyObject>.self)
  consume(UnsafeBufferPointer<AnyObject>.self)
  consume(UnsafeBufferPointer<Int8>.self)
  consume(UnsafePointer<Int8>.self)
}

@_specializeExtension
extension Array {

  @_specialize(exported: true,
               availability: SwiftStdlib 5.9, *;
               target: _endMutation(),
               where @_noMetadata Element : _Class)
  @available(SwiftStdlib 5.9, *)
  @usableFromInline
  mutating func __specialize_class__endMutation(){ Builtin.unreachable() }

  @_specialize(exported: true,
               availability: SwiftStdlib 5.9, *;
               target: _createNewBuffer(bufferIsUnique:minimumCapacity:growForAppend:),
               where @_noMetadata Element : _Class)
  @available(SwiftStdlib 5.9, *)
  @usableFromInline
  mutating func __specialize_class__createNewBuffer(bufferIsUnique: Bool, minimumCapacity: Int, growForAppend: Bool) { Builtin.unreachable() }

  @_specialize(exported: true,
               availability: SwiftStdlib 5.9, *;
               target: _makeUniqueAndReserveCapacityIfNotUnique(),
               where @_noMetadata Element : _Class)
  @available(SwiftStdlib 5.9, *)
  @usableFromInline
  mutating func __specialize_class__makeUniqueAndReserveCapacityIfNotUnique() { Builtin.unreachable() }

  @_specialize(exported: true,
               availability: SwiftStdlib 5.9, *;
               target: _appendElementAssumeUniqueAndCapacity(_:newElement:),
               where @_noMetadata Element : _Class)
  @available(SwiftStdlib 5.9, *)
  @usableFromInline
  mutating func __specialize_class__appendElementAssumeUniqueAndCapacity(_: Int, newElement: __owned Element) { Builtin.unreachable() }
}

#if _runtime(_ObjC)
@_specializeExtension
extension _ArrayBuffer {
  @_specialize(exported: true,
               availability: SwiftStdlib 5.9, *;
               target: _consumeAndCreateNew(bufferIsUnique:minimumCapacity:growForAppend:),
               where @_noMetadata Element : _Class)
  @available(SwiftStdlib 5.9, *)
  @usableFromInline
  func __specialize_class__consumeAndCreateNew(bufferIsUnique: Bool, minimumCapacity: Int, growForAppend: Bool) -> _ArrayBuffer<Element> { Builtin.unreachable() }

  @_specialize(exported: true,
               availability: SwiftStdlib 5.9, *;
               target: _copyContents(initializing:),
               where @_noMetadata Element : _Class)
  @available(SwiftStdlib 5.9, *)
  @usableFromInline
  __consuming func __specialize_class__copyContents(
    initializing buffer: UnsafeMutableBufferPointer<Element>
  ) -> (Iterator, UnsafeMutableBufferPointer<Element>.Index) { Builtin.unreachable() }

  @_specialize(exported: true,
               availability: SwiftStdlib 5.9, *;
               target: _copyContents(subRange:initializing:),
               where @_noMetadata Element : _Class)
  @available(SwiftStdlib 5.9, *)
  @usableFromInline
  __consuming func __specialize_class__copyContents(subRange: Range<Int>, initializing: Swift.UnsafeMutablePointer<Element>) -> Swift.UnsafeMutablePointer<Element> { Builtin.unreachable() }

  @_specialize(exported: true,
               availability: SwiftStdlib 5.9, *;
               target: _getElementSlowPath(_:),
               where @_noMetadata Element : _Class)
  @available(SwiftStdlib 5.9, *)
  func __specialize_class__getElementSlowPath(_ i: Int) -> AnyObject { Builtin.unreachable() }
}
#endif

@_specializeExtension
extension ContiguousArray {
  @_specialize(exported: true,
               availability: SwiftStdlib 5.9, *;
               target: _endMutation(),
               where @_noMetadata Element : _Class)
  @available(SwiftStdlib 5.9, *)
  @usableFromInline
  mutating func __specialize_class__endMutation(){ Builtin.unreachable() }

  @_specialize(exported: true,
               availability: SwiftStdlib 5.9, *;
               target: _createNewBuffer(bufferIsUnique:minimumCapacity:growForAppend:),
               where @_noMetadata Element : _Class)
  @available(SwiftStdlib 5.9, *)
  @usableFromInline
  mutating func __specialize_class__createNewBuffer(bufferIsUnique: Bool, minimumCapacity: Int, growForAppend: Bool) { Builtin.unreachable() }

  @_specialize(exported: true,
               availability: SwiftStdlib 5.9, *;
               target: _makeUniqueAndReserveCapacityIfNotUnique(),
               where @_noMetadata Element : _Class)
  @available(SwiftStdlib 5.9, *)
  @usableFromInline
  mutating func __specialize_class__makeUniqueAndReserveCapacityIfNotUnique() { Builtin.unreachable() }

  @_specialize(exported: true,
               availability: SwiftStdlib 5.9, *;
               target: _appendElementAssumeUniqueAndCapacity(_:newElement:),
               where @_noMetadata Element : _Class)
  @available(SwiftStdlib 5.9, *)
  @usableFromInline
  mutating func __specialize_class__appendElementAssumeUniqueAndCapacity(_: Int, newElement: __owned Element) { Builtin.unreachable() }

  @_specialize(exported: true,
               availability: SwiftStdlib 5.9, *;
              target: _reserveCapacityImpl(minimumCapacity:growForAppend:),
              where @_noMetadata Element : _Class)
  @available(SwiftStdlib 5.9, *)
  @usableFromInline
  mutating func __specialize_class__reserveCapacityImpl(minimumCapacity: Int, growForAppend: Bool) { Builtin.unreachable() }

  @_specialize(exported: true,
               availability: SwiftStdlib 5.9, *;
              target: _reserveCapacityAssumingUniqueBuffer(oldCount:),
              where @_noMetadata Element : _Class)
  @available(SwiftStdlib 5.9, *)
  @usableFromInline
  mutating func __specialize_class__reserveCapacityAssumingUniqueBuffer(oldCount: Int) { Builtin.unreachable() }

  @_specialize(exported: true,
               availability: SwiftStdlib 5.9, *;
              target: reserveCapacity(_:),
              where @_noMetadata Element : _Class)
  @available(SwiftStdlib 5.9, *)
  @usableFromInline
  mutating func __specialize_class__reserveCapacity(_ minimumCapacity: Int) { Builtin.unreachable() }
}

@_specializeExtension
extension _ContiguousArrayBuffer {
  @_specialize(exported: true,
               availability: SwiftStdlib 5.9, *;
               target: _consumeAndCreateNew(bufferIsUnique:minimumCapacity:growForAppend:),
               where @_noMetadata Element : _Class)
  @available(SwiftStdlib 5.9, *)
  @usableFromInline
  func __specialize_class__consumeAndCreateNew(bufferIsUnique: Bool, minimumCapacity: Int, growForAppend: Bool) -> _ContiguousArrayBuffer<Element> { Builtin.unreachable() }
}
