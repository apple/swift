#import <Foundation/Foundation.h>

struct S {
  NSString *_Nullable A;
  NSString *_Nullable B;
  NSString *_Nullable C;

#ifdef S_NONTRIVIAL_DESTRUCTOR
  ~S() {}
#endif

  void dump() const {
    printf("%s\n", [A UTF8String]);
    printf("%s\n", [B UTF8String]);
    printf("%s\n", [C UTF8String]);
  }
};

inline void takeSFunc(S s) {
  s.dump();
}

struct NonTrivialLogDestructor {
    int x = 0;

    ~NonTrivialLogDestructor() {
        printf("~NonTrivialLogDestructor %d\n", x);
    }
};

@interface ClassWithNonTrivialDestructorIvar: NSObject

- (ClassWithNonTrivialDestructorIvar * _Nonnull)init;

- (void)takesS:(S)s;

@end

struct ReferenceStructToClassWithNonTrivialLogDestructorIvar {
    ClassWithNonTrivialDestructorIvar *_Nonnull x;

#ifdef S_NONTRIVIAL_DESTRUCTOR
    ~ReferenceStructToClassWithNonTrivialLogDestructorIvar() {}
#endif
};
