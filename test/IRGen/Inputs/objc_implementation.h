#import <Foundation/Foundation.h>

@interface ImplClass: NSObject <NSCopying>

- (nonnull instancetype)init;

@property (assign) int implProperty;

- (void)mainMethod:(int)param;

@end


@interface ImplClass (Category1)

- (void)category1Method:(int)param;

@end


@interface ImplClass (Category2)

- (void)category2Method:(int)param;

@end


@interface NoImplClass

- (void)noImplMethod:(int)param;

@end

@interface NoInitImplClass: NSObject

@property (readonly, strong, nonnull) NSString *s1;
@property (strong, nonnull) NSString *s2;
@property (readonly, strong, nonnull) NSString *s3;
@property (strong, nonnull) NSString *s4;

@end
