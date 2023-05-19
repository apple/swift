#import <Foundation.h>
#import <CoreFoundation.h>
@import Dispatch;
#include "other-header.h"

#define MACRO_GOT_UNDEFINED 1
#undef MACRO_GOT_UNDEFINED
#define MY_MACRO 1

#define MACRO_DUP 2
#define MACRO_DUP 3

void doSomethingInHead(int arg);

@interface BaseInHead
- (void)doIt:(int)arg;
@end

/// Awesome name.
@interface SameName
@end
@protocol SameName
@end

@interface BaseInHead(SomeCategory)
-(void)doItInCategory;
@end

@protocol Superproto
-(void)lala;
@end

@class Cake;
struct Arkham;
@protocol Soul;

typedef struct __attribute__((objc_bridge(id))) __MyLittleCFType *MyLittleCFType;
