@import Foundation;
@import Alpha;

#define MAIN_ACTOR __attribute__((swift_attr("@MainActor")))

@interface BaseNonisolated : NSObject
@end
@interface DerivedNonisolated : BaseNonisolated
@end

MAIN_ACTOR
@interface BaseIsolatedClass : NSObject
@end
@interface DerivedIsolatedClass : BaseIsolatedClass
@end

@interface BaseIsolatedDealloc : NSObject
- (void)dealloc MAIN_ACTOR;
@end
@interface DerivedIsolatedDealloc : BaseIsolatedDealloc
@end

@protocol DeallocP
- (void)dealloc MAIN_ACTOR;
@end

@interface DeallocIsolatedFromProtocol : NSObject <DeallocP>
@end

@interface DeallocIsolatedFromCategory : NSObject
@end

@interface DeallocIsolatedFromCategory (Extra)
- (void)dealloc MAIN_ACTOR;
@end

@interface DeallocIsolatedFromExtension : NSObject
@end

@interface DeallocIsolatedFromExtension ()
- (void)dealloc MAIN_ACTOR;
@end
