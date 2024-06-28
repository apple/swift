// RUN: %empty-directory(%t)
// RUN: %{python} %utils/split_file.py -o %t %s

// RUN: %target-swift-emit-ir %t/Main.swift -import-bridging-header %t/BridgingHeader.h -parse-as-library -enable-experimental-feature Embedded -wmo \
// RUN:  -target armv7em-none-none-eabi -Xcc -mthumb -Xcc -mcpu=cortex-m7 -Xcc -mfloat-abi=hard -Xcc -mfpu=fpv5-sp-d16 -Xcc -D__FPU_USED=1 -Xcc -falign-functions=16

// REQUIRES: swift_in_compiler
// REQUIRES: optimized_stdlib
// REQUIRES: OS=macosx || OS=linux-gnu
// REQUIRES: CODEGENERATOR=ARM
// REQUIRES: embedded_stdlib_cross_compiling

// BEGIN BridgingHeader.h

typedef struct
{
  float x;
  float y;
  float width;
  float height;
} Rect;

typedef void FunctionType(Rect, Rect);
void (* _Nonnull callback)(FunctionType * _Nonnull func);
void c_function(Rect, Rect);

// BEGIN Main.swift

@main
struct Main {
  static func main() {
    callback({ a, b in
      c_function(a, b)
    })
  }
}
