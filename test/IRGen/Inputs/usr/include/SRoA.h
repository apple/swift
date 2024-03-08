
typedef struct S {
  float f;
  float g;
} S;

void f(S);

#if defined(__OBJC__)
@interface I
- (instancetype _Nonnull)initWithS:(S)s;
@end
#endif

