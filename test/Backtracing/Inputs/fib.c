#include <stdio.h>
#include <stdlib.h>

int fib(int x) {
  if (x < 2) {
    if (x == 0) {
      return 0;
    }
    return 1;
  }

  return fib(x - 1) + fib(x - 2);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr,
            "usage: fib <N> [<M>...]\n"
            "\n"
            "Return the Nth fibonacci number.\n");
    return 0;
  }

  for (int n = 1; n < argc; ++n) {
    int x = atoi(argv[n]);

    printf("%d: %d\n", x, fib(x));
  }

  return 0;
}
