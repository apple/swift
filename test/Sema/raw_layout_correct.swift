// RUN: %target-swift-frontend -enable-experimental-feature RawLayout -typecheck -verify %s

@_rawLayout(like: T)
struct RawStorage<T>: ~Copyable {}

@_rawLayout(likeArrayOf: T, count: 4)
struct RawSmallArray<T>: ~Copyable {}

@_rawLayout(size: 4, alignment: 4)
struct Lock: ~Copyable {}
