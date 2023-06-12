// RUN: %target-swift-frontend %s -Onone -emit-ir -g -o - -parse-as-library -module-name a | %FileCheck %s
func use<T>(_ t : T) {}
public func f() -> (() -> ()) {
    lazy var i : Int = 0
  return {
      // CHECK: call {{.*}} @"$s1a1fyycyF1iL_Sivg"({{.*}}), !dbg ![[GETTER_LOC:[0-9]+]]
      // CHECK: ![[GETTER_LOC]] = !DILocation(line: [[@LINE+1]], column: 11
      use(i)
    }
}
