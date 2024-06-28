// rdar://129195380 - Make sure we correctly handle '#if' when skipping function
// bodies.
class C {
  func test1() {
  #if FOOBAR
// RUN: %sourcekitd-test -req=cursor -pos=%(line + 2):5 %s -- %s -DFOOBAR
// RUN: %sourcekitd-test -req=cursor -pos=%(line + 1):5 %s -- %s
    abc
  }

  func test2() {
  }
}
