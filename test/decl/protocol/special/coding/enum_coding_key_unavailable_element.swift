// RUN: %target-run-simple-swift
// RUN: %target-run-simple-swift(-Xfrontend -unavailable-decl-optimization=complete)

// REQUIRES: executable_test

enum Key: CodingKey {
    case a
    @available(*, unavailable) case b
}

guard Key(stringValue: "a") == .a else { fatalError() }
guard Key(stringValue: "b") == nil else { fatalError() }

enum StringKey: String, CodingKey {
    case x
    @available(*, unavailable) case y
}

guard StringKey(stringValue: "x") == .x else { fatalError() }
guard StringKey(stringValue: "y") == nil else { fatalError() }
guard StringKey(intValue: 0) == nil else { fatalError() }
guard StringKey(intValue: 1) == nil else { fatalError() }

enum IntKey: Int, CodingKey {
    case zero = 0
    @available(*, unavailable) case one = 1
}

guard IntKey(stringValue: "zero") == .zero else { fatalError() }
guard IntKey(stringValue: "one") == nil else { fatalError() }
guard IntKey(intValue: 0) == .zero else { fatalError() }
guard IntKey(intValue: 1) == nil else { fatalError() }
