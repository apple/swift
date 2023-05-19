public extension Regular {
  __consuming func shutdownParty() {
    // FIXME: rdar://108933330 (cannot define struct deinit with -enable-library-evolution)
//    discard self // ok; same module
  }
}

public extension Frozen {
  __consuming func shutdownParty() {
    discard self // ok; same module
  }
}
