// RUN: %target-swift-frontend -sil-verify-all -verify -emit-sil -enable-experimental-feature MoveOnlyEnumDeinits %s
// RUN: %target-swift-frontend  -enable-experimental-feature NoncopyableGenerics -sil-verify-all -verify -emit-sil -enable-experimental-feature MoveOnlyEnumDeinits %s


func posix_close(_ t: Int) {}

struct GoodFileDescriptor: ~Copyable {
  let _fd: Int = 0

  var rawFileDescriptor: Int {
    __consuming get {
      let x = _fd
      discard self
      return x
    }
  }

  __consuming func close() {
    posix_close(_fd)
    discard self
  }

  deinit { // expected-error {{'self' consumed more than once}}
    // FIXME: this is suppose to be valid. rdar://106044273
    close() // expected-note {{consumed here}}
  } // expected-note {{consumed again here}}
}

struct BadFileDescriptor: ~Copyable {
  let _fd: Int = 0

  deinit {}

  var rawFileDescriptor: Int {
    __consuming get { // expected-error {{'self' consumed more than once}}
      discard self    // expected-note {{consumed here}}
      return self.rawFileDescriptor  // expected-note {{consumed again here}}
                                     // expected-warning@-1 {{function call causes an infinite recursion}}
    }
  }

  __consuming func closeBoring(_ b: Bool) -> Int { // expected-error {{'self' consumed more than once}}
    if b {
      discard self // expected-note {{consumed here}}
    }
    return rawFileDescriptor // expected-note {{consumed again here}}
  }

  __consuming func closeRepeatedly(_ n: Int) -> Int { // expected-error {{'self' used after consume}}
    for _ in 0..<n {
      posix_close(_fd) // expected-note {{used here}}
      discard self     // expected-note {{consumed here}}
    }
    return 0
  }
}

final class Wallet {
  var ticket1: Ticket = .green
}

enum Ticket: ~Copyable {
  case green
  case yellow
  case red
  case pagedOut(GoodFileDescriptor)
  case within(Wallet)

  consuming func discard() { discard self }

  init(inWallet wallet: Wallet? = nil) { // expected-error {{'self' consumed more than once}}
    self = .within(Wallet())
    if let existingWallet = wallet {
      discard()
      self = .within(existingWallet)
    }
    discard(forever: true) // expected-note {{consumed here}}
  } // expected-note {{consumed again here}}

  __consuming func discard(forever: Bool) { // expected-error {{'self' consumed in a loop}}
    while forever {
      discard self // expected-note {{consumed here}}
    }
  }

  __consuming func inspect() { // expected-error {{'self' consumed more than once}}
    switch consume self { // expected-note {{consumed here}}
    // TODO: case patterns with shared block rdar://125188955
    /*
    case .green, .yellow, .red:
      discard self // e/xpected-note {{consumed again here}}
      */
    case .green:
      discard self
    case .yellow:
      discard self
    case .red:
      discard self // expected-note {{consumed again here}}
    default:
      return
    }
  }

  deinit {}

}
