#import <Foundation/Foundation.h>
#import "cxx-class-with-arc-fields-ctor.h"

@implementation ClassWithNonTrivialDestructorIvar {
    NonTrivialLogDestructor value;
};

- (ClassWithNonTrivialDestructorIvar *)init {
    self->value.x = 21;
    return self;
}

- (void)takesS:(S)s {
    printf("takesS!\n");
    s.dump();
}

@end
