@import Foundation;

struct StructOfNSStrings {
  __unsafe_unretained NSString *nsstr;
};

struct StructOfBlocks {
  void (^__unsafe_unretained _Nonnull block)(void);
};

@interface MYObject: NSObject
@end

struct StrongsInAStruct {
  __strong MYObject *_Nonnull myobj;
};

struct WeaksInAStruct {
  // Weak pointers are always _Nullable
  __weak MYObject *_Nullable myobj;
};

struct WeakAndNonnull {
  // Weak pointers cannnnn be annotated _Nonnull
  __weak MYObject *_Nonnull myobj;
};
