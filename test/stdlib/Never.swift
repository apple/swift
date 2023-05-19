// RUN: %target-typecheck-verify-swift

struct ConformsToError<T: Error> {}
_ = ConformsToError<Never>()

struct ConformsToEquatable<T: Equatable> {}
_ = ConformsToEquatable<Never>()

struct ConformsToComparable<T: Comparable> {}
_ = ConformsToComparable<Never>()

struct ConformsToHashable<T: Hashable> {}
_ = ConformsToHashable<Never>()

if #available(SwiftStdlib 5.5, *) {
  struct ConformsToIdentifiable<T: Identifiable> {}
  _ = ConformsToIdentifiable<Never>()
}

if #available(SwiftStdlib 5.9, *) {
  struct ConformsToCodable<T: Codable> {}
  _ = ConformsToCodable<Never>()
}
