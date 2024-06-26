// RUN: %target-typecheck-verify-swift -Xllvm -swift-diagnostics-assert-on-warning=1

infix operator %%%%%
prefix operator **-
postfix operator **+

func %%%%% (lhs: Int, rhs: Int, file: String = #file) -> String { file } // no-err
prefix func **- (a: Int, file: String = #file) -> String { file } // no-err
postfix func **+ (a: Int, file: String = #file) -> String { file } // no-err

print(3 %%%%% 4) // no-err
print(**-3) // no-err
print(3**+) // no-err