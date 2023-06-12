func square(_ x: Int) -> Int {
  return x * x
}

func euclid2(_ a: Int, _ b: Int) -> Int {
  return square(a) + square(b)
}

@main
struct Inlining {
  static func main() {
    if CommandLine.argc != 3 {
      print("usage: Inlining <a> <b>")
      exit(1)
    }

    guard let a = Int(CommandLine.arguments[1]) else {
      print("Argument <a> must be a number")
      exit(1)
    }
    guard let b = Int(CommandLine.arguments[2]) else {
      print("Argument <b> must be a number")
      exit(1)
    }
    let result = euclid2(a, b)

    print("\(a) * \(a) + \(b) * \(b) = \(result)")
  }
}
