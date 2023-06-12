// NOTE: This file is sensitive to line numbers.  Thus RUN and CHECK lines come
// below the code.
//
// NOTE: Please don't change this file to use FileCheck's feature to match
// relative line numbers: those lines are comments and we don't want to see
// anything extra in a test for documentation comments.




//===--- Check how we merge consecutive comments.

// not_doc1  NOT_DOC
func not_doc1() {}

/* not_doc2  NOT_DOC */
func not_doc2() {}

//! Doxygen comment, but not Swift.  not_doc3  NOT_DOC
func not_doc3() {}

/*! Doxygen comment, but not Swift.  not_doc4  NOT_DOC */
func not_doc4() {}

/*/ not_doc5  NOT_DOC */
func not_doc5() {}

/** is_doc6 IS_DOC_SINGLE */
func is_doc6() {}

/**
 * is_doc7  IS_DOC_SINGLE */
func is_doc7() {}

/**
 * is_doc8
 * IS_DOC_SINGLE */
func is_doc8() {}

/**
 * is_doc9
 * IS_DOC_SINGLE
 */
func is_doc9() {}

/// is_doc10 IS_DOC_SINGLE
func is_doc10() {}

// Aaa.  is_doc11 NOT_DOC
/// Bbb.  is_doc11 IS_DOC_SINGLE
func is_doc11() {}

/// is_doc12 IS_DOC_SINGLE
// Not a Doxygen comment.  NOT_DOC
func is_doc12() {}

/** is_doc13 IS_DOC_SINGLE */
/* Not a Doxygen comment.  not_doc13  NOT_DOC */
func is_doc13() {}

/// is_doc14 IS_DOC_START
/// IS_DOC_END
func is_doc14() {}

/// is_doc15 IS_DOC_START
/// Aaa bbb ccc.
/// IS_DOC_END
func is_doc15() {}

/// IS_DOC_NOT_ATTACHED
/// Should not return with the comments for the function below it or prevent
/// its actual comment from being returned.

/// IS_DOC_START priorCommentOneLineGap Aaa.
///
/// Bbb. IS_DOC_END
func priorCommentOneLineGap() {}

/// IS_DOC_NOT_ATTACHED
/// Same as above but with a two line gap this time.


/// IS_DOC_START priorCommentTwoLineGap Aaa.
///
/// Bbb. IS_DOC_END
func priorCommentTwoLineGap() {}

/// IS_DOC_NOT_ATTACHED
/// Make sure a gyb comment doesn't cause the previous comment to stay
/// attached.
// ###line 9001

/// IS_DOC_START priorCommentGyb Aaa.
///
/// Bbb. IS_DOC_END
func priorCommentGyb() {}

/**
  IS_DOC_START priorBlockMultiLineComment Aaa.
 */
/**
  Bbb. IS_DOC_END
 */
func priorBlockMultiLineComment() {}

/** IS_DOC_START priorBlockSingleLineComment Aaa. */
/**
  Bbb. IS_DOC_END
 */
func priorBlockSingleLineComment() {}

/** IS_DOC_START priorBlockMixedComment Aaa. */
/// Bbb.
/// Multiline. IS_DOC_END
func priorBlockMixedComment() {}

/// IS_DOC_START priorLineMixedComment Aaa.
/// Multiline.
/**  Bbb. IS_DOC_END */
func priorLineMixedComment() {}

/// IS_DOC_START priorSingleLineMixedComment Aaa.
/**
  Bbb. IS_DOC_END
 */
func priorSingleLineMixedComment() {}

/**
  IS_DOC_START priorBlockMultiLineMixedComment Aaa.
 */
/// Bbb. IS_DOC_END
func priorBlockMultiLineMixedComment() {}

/// priorCommentBlankLineBeforeDecl IS_DOC_SINGLE

func priorCommentBlankLineBeforeDecl() {}

/// priorCommentBrokenLineBeforeLineComment IS_DOC_SINGLE

// NOT_DOC
func priorCommentBrokenBeforeLineComment() {}

// Aaa. NOT_DOC
/// IS_DOC_START allTheThings
/** Bbb
 *
 * Ccc. */
// Ddd. NOT_DOC
/// Eee
/**
 * Fff. IS_DOC_END
 */
func allTheThings() {}

// RUN: %target-swift-ide-test -print-comments -source-filename %s > %t.txt
// RUN: %FileCheck %s -check-prefix=WRONG < %t.txt
// RUN: %FileCheck %s < %t.txt

// Non-documentation comments should not be attached to anything.
// WRONG-NOT: NOT_DOC

// Some comments are not attached to anything.
// WRONG-NOT: IS_DOC_NOT_ATTACHED

// Ensure we don't pick up extra comments.
// WRONG-NOT: RawComment={{.*}}IS_DOC_START{{.*}}IS_DOC_START{{.*}}BriefComment=
// WRONG-NOT: RawComment={{.*}}IS_DOC_END{{.*}}IS_DOC_END{{.*}}BriefComment=

// CHECK: comment_merge.swift:14:6: Func/not_doc1 RawComment=none
// CHECK-NEXT: comment_merge.swift:17:6: Func/not_doc2 RawComment=none
// CHECK-NEXT: comment_merge.swift:20:6: Func/not_doc3 RawComment=none
// CHECK-NEXT: comment_merge.swift:23:6: Func/not_doc4 RawComment=none
// CHECK-NEXT: comment_merge.swift:26:6: Func/not_doc5 RawComment=none
// CHECK-NEXT: comment_merge.swift:29:6: Func/is_doc6 RawComment=[/** is_doc6 IS_DOC_SINGLE */]
// CHECK-NEXT: comment_merge.swift:33:6: Func/is_doc7 RawComment=[/**\n * is_doc7  IS_DOC_SINGLE */]
// CHECK-NEXT: comment_merge.swift:38:6: Func/is_doc8 RawComment=[/**\n * is_doc8\n * IS_DOC_SINGLE */]
// CHECK-NEXT: comment_merge.swift:44:6: Func/is_doc9 RawComment=[/**\n * is_doc9\n * IS_DOC_SINGLE\n */]
// CHECK-NEXT: comment_merge.swift:47:6: Func/is_doc10 RawComment=[/// is_doc10 IS_DOC_SINGLE\n]
// CHECK-NEXT: comment_merge.swift:51:6: Func/is_doc11 RawComment=[/// Bbb.  is_doc11 IS_DOC_SINGLE\n]
// CHECK-NEXT: comment_merge.swift:55:6: Func/is_doc12 RawComment=[/// is_doc12 IS_DOC_SINGLE\n]
// CHECK-NEXT: comment_merge.swift:59:6: Func/is_doc13 RawComment=[/** is_doc13 IS_DOC_SINGLE */]
// CHECK-NEXT: comment_merge.swift:63:6: Func/is_doc14 RawComment=[/// is_doc14 IS_DOC_START\n/// IS_DOC_END\n]
// CHECK-NEXT: comment_merge.swift:68:6: Func/is_doc15 RawComment=[/// is_doc15 IS_DOC_START\n/// Aaa bbb ccc.\n/// IS_DOC_END\n]
// CHECK-NEXT: comment_merge.swift:77:6: Func/priorCommentOneLineGap RawComment=[/// IS_DOC_START priorCommentOneLineGap Aaa.\n///\n/// Bbb. IS_DOC_END\n]
// CHECK-NEXT: comment_merge.swift:86:6: Func/priorCommentTwoLineGap RawComment=[/// IS_DOC_START priorCommentTwoLineGap Aaa.\n///\n/// Bbb. IS_DOC_END\n]
// CHECK-NEXT: comment_merge.swift:96:6: Func/priorCommentGyb RawComment=[/// IS_DOC_START priorCommentGyb Aaa.\n///\n/// Bbb. IS_DOC_END\n]
// CHECK-NEXT: comment_merge.swift:104:6: Func/priorBlockMultiLineComment RawComment=[/**\n  IS_DOC_START priorBlockMultiLineComment Aaa.\n *//**\n  Bbb. IS_DOC_END\n */]
// CHECK-NEXT: comment_merge.swift:110:6: Func/priorBlockSingleLineComment RawComment=[/** IS_DOC_START priorBlockSingleLineComment Aaa. *//**\n  Bbb. IS_DOC_END\n */]
// CHECK-NEXT: comment_merge.swift:115:6: Func/priorBlockMixedComment RawComment=[/** IS_DOC_START priorBlockMixedComment Aaa. *//// Bbb.\n/// Multiline. IS_DOC_END\n]
// CHECK-NEXT: comment_merge.swift:120:6: Func/priorLineMixedComment RawComment=[/// IS_DOC_START priorLineMixedComment Aaa.\n/// Multiline.\n/**  Bbb. IS_DOC_END */]
// CHECK-NEXT: comment_merge.swift:126:6: Func/priorSingleLineMixedComment RawComment=[/// IS_DOC_START priorSingleLineMixedComment Aaa.\n/**\n  Bbb. IS_DOC_END\n */]
// CHECK-NEXT: comment_merge.swift:132:6: Func/priorBlockMultiLineMixedComment RawComment=[/**\n  IS_DOC_START priorBlockMultiLineMixedComment Aaa.\n *//// Bbb. IS_DOC_END\n]
// CHECK-NEXT: comment_merge.swift:136:6: Func/priorCommentBlankLineBeforeDecl RawComment=[/// priorCommentBlankLineBeforeDecl IS_DOC_SINGLE\n]
// CHECK-NEXT: comment_merge.swift:141:6: Func/priorCommentBrokenBeforeLineComment RawComment=[/// priorCommentBrokenLineBeforeLineComment IS_DOC_SINGLE\n]
// CHECK-NEXT: comment_merge.swift:153:6: Func/allTheThings RawComment=[/// IS_DOC_START allTheThings\n/** Bbb\n *\n * Ccc. *//// Eee\n/**\n * Fff. IS_DOC_END\n */]
