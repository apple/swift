// RUN: %target-typecheck-verify-swift

// rdar://113745963

// https://github.com/apple/swift/pull/67441 changed closure resolution order which makes the following code fail to type-check.

struct Date : Equatable {
  static var distantPast = Date()
}

enum Request {
  struct Options: OptionSet {
    static let option1 = Options(rawValue: 1 << 0)
    static let option2 = Options(rawValue: 1 << 1)

    let rawValue: Int

    init(rawValue: Int) {
      self.rawValue = rawValue
    }
  }

  enum Source : Comparable {
    case automatic
    case manual(Date)

    static func < (lhs: Source, rhs: Source) -> Bool { true }
  }

  case problem(options: Options, source: Source)
}

enum OuterSource {
  case automatic, manual, unknown
}

struct Test {
  func test(arr: [Int]) {
    let _: [Request] = arr.map { value in
      let source: OuterSource = .automatic
      let dateAdded = Date.distantPast
      return .problem(
             options: {
               switch source {
               case .automatic:
                 return [.option1, .option2]
               case .manual, .unknown:
                 return []
               }
             }(),
             source: {
               switch source {
               case .manual:
                 return .manual(dateAdded)
               case .automatic, .unknown:
                 return .automatic
               }
             }())
    }
  }
}
