// RUN: not %scale-test --begin 1 --end 3 --step 1 --select incrementScopeCounter %s
// REQUIRES: OS=macosx
// REQUIRES: asserts

func rdar33289839(s: String) -> String {
  return "test" + String(s)
%for i in range(0, N):
       + "test" + String(s)
%end
}
