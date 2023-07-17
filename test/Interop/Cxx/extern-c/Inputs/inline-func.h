extern "C" {

inline void inlineFn();

void cacheMis() { }
void incorrectCacheHit() {
  inlineFn();
}

static void caller() {
  cacheMis();
  incorrectCacheHit();
}

inline void inlineFn() { }

}

