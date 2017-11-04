func testExpandTernaryExpr() {
  let a = 3
  let b = 5
  let x: Int = a < 5 ? a : b
}
func testExpandMultilineTernaryExpr() {
  let a = 3
  let b = 5
  let (x, y) = a < 5
    ? (a, b)
    : (b, a)
}
func testExpandAssignOnlyTernaryExpr() {
  let a = 3
  let b = 5
  let x: Int
  x = a < 5 ? a : b
}
func testExpandAssignOnlyTernaryExpr() {
  let a = 3
  let b = 5
  let x: Int
  let y: Int
  if a < 5 {
    (x, y) = (a, b)
  } else {
    (x, y) = (b, a)
  }
}
