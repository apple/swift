#import <Foundation/Foundation.h>

@interface Foo: NSObject

- (CFTypeRef _Nonnull)bar;
- (CFTypeRef _Nullable)nullabar;

@end

void printNullableRef(CFTypeRef _Nullable ref);
void printNonnullRef(CFTypeRef _Nonnull ref);
