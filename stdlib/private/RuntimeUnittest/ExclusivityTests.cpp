#include "swift/Runtime/Exclusivity.h"
#include "swift/Runtime/Metadata.h"

using namespace swift;

SWIFT_CC SWIFT_RUNTIME_LIBRARY_VISIBILITY extern "C"
void testExclusivityNullPC() {
  ValueBuffer scratch, scratch2;
  long var;
  swift_beginAccess(&var, &scratch,
                    ExclusivityFlags::Read | ExclusivityFlags::Tracking,
                    /*pc=*/0);
  swift_beginAccess(&var, &scratch2, ExclusivityFlags::Modify,
                    /*pc=*/0);
  swift_endAccess(&scratch2);
  swift_endAccess(&scratch);
}

SWIFT_CC SWIFT_RUNTIME_LIBRARY_VISIBILITY extern "C"
void testExclusivityPCOne() {
  ValueBuffer scratch, scratch2;
  long var;
  swift_beginAccess(&var, &scratch,
                    ExclusivityFlags::Read | ExclusivityFlags::Tracking,
                    /*pc=*/(void *)1);
  swift_beginAccess(&var, &scratch2, ExclusivityFlags::Modify,
                    /*pc=*/(void *)1);
  swift_endAccess(&scratch2);
  swift_endAccess(&scratch);
}

SWIFT_CC SWIFT_RUNTIME_LIBRARY_VISIBILITY extern "C"
void testExclusivityBogusPC() {
  ValueBuffer scratch, scratch2;
  long var;
  swift_beginAccess(&var, &scratch,
                    ExclusivityFlags::Read | ExclusivityFlags::Tracking,
                    /*pc=*/(void *)0xdeadbeefdeadbeefULL);
  swift_beginAccess(&var, &scratch2, ExclusivityFlags::Modify,
                    /*pc=*/(void *)0xdeadbeefdeadbeefULL);
  swift_endAccess(&scratch2);
  swift_endAccess(&scratch);
}


// rdar://32866493
SWIFT_CC SWIFT_RUNTIME_LIBRARY_VISIBILITY extern "C"
void testExclusivityNonNested() {
  const int N = 5;
  ValueBuffer scratches[N];
  long vars[N];

  auto begin = [&](unsigned i) {
    assert(i < N);
    swift_beginAccess(&vars[i], &scratches[i],
                      ExclusivityFlags::Modify | ExclusivityFlags::Tracking, 0);
  };
  auto end = [&](unsigned i) {
    assert(i < N);
    swift_endAccess(&scratches[i]);
    memset(&scratches[i], /*gibberish*/ 0x99, sizeof(ValueBuffer));
  };
  auto accessAll = [&] {
    for (unsigned i = 0; i != N; ++i) begin(i);
    for (unsigned i = 0; i != N; ++i) end(i);
  };

  accessAll();
  begin(0); begin(1); end(0); end(1);
  begin(0); begin(1); end(0); end(1);
  accessAll();
  begin(1); begin(0); begin(2); end(0); end(2); end(1);
  accessAll();
  begin(0); begin(1); begin(2); begin(3); begin(4);
  end(1); end(4); end(0); end(2); end(3);
  accessAll();
}
