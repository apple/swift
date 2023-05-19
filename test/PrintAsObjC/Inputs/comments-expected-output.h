SWIFT_CLASS("_TtC8comments4A000")
@interface A000
@end


/// Aaa.  A010.  Bbb.
SWIFT_CLASS("_TtC8comments21A010_AttachToEntities")
@interface A010_AttachToEntities
/// Aaa.  init().
- (nonnull instancetype)init OBJC_DESIGNATED_INITIALIZER;
- (NSInteger)objectAtIndexedSubscript:(NSInteger)i SWIFT_WARN_UNUSED_RESULT;
- (void)setObject:(NSInteger)newValue atIndexedSubscript:(NSInteger)i;
/// Aaa.  v1.
@property (nonatomic) NSInteger v1;
/// Aaa.  v2.
SWIFT_CLASS_PROPERTY(@property (nonatomic, class, readonly) NSInteger v2;)
+ (NSInteger)v2 SWIFT_WARN_UNUSED_RESULT;
@end


/// Aaa.  A013.
SWIFT_PROTOCOL("_TtP8comments21A013_AttachToEntities_")
@protocol A013_AttachToEntities
@end


SWIFT_CLASS("_TtC8comments10ATXHeaders")
@interface ATXHeaders
/// <h1>LEVEL ONE</h1>
/// <h2>LEVEL TWO</h2>
- (void)f0;
@end


SWIFT_CLASS("_TtC8comments10Attributes")
@interface Attributes
/// Here is an attribute:
/// Attribute text
- (void)f0;
@end


SWIFT_CLASS("_TtC8comments13AutomaticLink")
@interface AutomaticLink
/// And now for a URL.
/// <a href="http://developer.apple.com/swift/">http://developer.apple.com/swift/</a>
- (void)f0;
@end


SWIFT_CLASS("_TtC8comments10BlockQuote")
@interface BlockQuote
/// Aaa.
/// <blockquote>
/// Bbb.
///
/// </blockquote>
/// <blockquote>
/// Ccc.
///
/// </blockquote>
- (void)f0;
@end


SWIFT_CLASS("_TtC8comments5Brief")
@interface Brief
/// Aaa.
- (void)f0;
/// Aaa.
/// Bbb.
- (void)f1;
/// Aaa.
/// <blockquote>
/// Bbb.
///
/// </blockquote>
- (void)f2;
/// Aaa.
/// Bbb.
- (void)f3;
@end


SWIFT_CLASS("_TtC8comments15ClosingComments")
@interface ClosingComments
/// Some comment. */
- (void)closingComment;
@end


SWIFT_CLASS("_TtC8comments16ClosureContainer")
@interface ClosureContainer
/// Partially applies a binary operator.
/// \param a The left-hand side to partially apply.
///
/// \param combine A binary operator.
///
/// \a combine parameters:
/// <ul>
/// <li>
/// lhs: The left-hand side of the operator
/// </li>
/// <li>
/// rhs: The right-hand side of the operator
/// </li>
/// </ul>
///
///
/// \a combine returns: A result.
///
/// \a combine error: Nothing.
///
- (void)closureParameterExplodedExplodedWithA:(NSInteger)a combine:(SWIFT_NOESCAPE NSInteger (^ _Nonnull)(NSInteger, NSInteger))combine;
/// Partially applies a binary operator.
/// \param a The left-hand side to partially apply.
///
/// \param combine A binary operator.
///
/// \a combine parameters:
/// <ul>
/// <li>
/// lhs: The left-hand side of the operator
/// </li>
/// <li>
/// rhs: The right-hand side of the operator
/// </li>
/// </ul>
///
///
/// \a combine returns: A result.
///
/// \a combine error: Nothing.
///
- (void)closureParameterOutlineExplodedWithA:(NSInteger)a combine:(SWIFT_NOESCAPE NSInteger (^ _Nonnull)(NSInteger, NSInteger))combine;
/// Partially applies a binary operator.
/// \param a The left-hand side to partially apply.
///
/// \param combine A binary operator.
///
/// \a combine parameters:
/// <ul>
/// <li>
/// lhs: The left-hand side of the operator
/// </li>
/// <li>
/// rhs: The right-hand side of the operator
/// </li>
/// </ul>
///
///
/// \a combine returns: A result.
///
/// \a combine error: Nothing.
///
- (void)closureParameterOutlineOutlineWithA:(NSInteger)a combine:(SWIFT_NOESCAPE NSInteger (^ _Nonnull)(NSInteger, NSInteger))combine;
@end


SWIFT_CLASS("_TtC8comments9CodeBlock")
@interface CodeBlock
/// This is how you use this code.
/// \code
/// f0() // WOW!
/// f0() // WOW!
/// f0() // WOW!
///
/// \endcode
- (void)f0;
@end


SWIFT_CLASS("_TtC8comments8Emphasis")
@interface Emphasis
/// Aaa <em>bbb</em> ccc.
/// Aaa <em>bbb</em> ccc.
- (void)f0;
@end


SWIFT_CLASS("_TtC8comments13EmptyComments")
@interface EmptyComments
///
- (void)f0;
/// Aaa.
- (void)f1;
///
- (void)f2;
///
- (void)f3;
/// Aaa.
- (void)f4;
@end


SWIFT_CLASS("_TtC8comments9Footnotes")
@interface Footnotes
/// Has some footnotes.
/// Footnotes aren’t handled by swiftMarkup yet[^footnote], but they may in the future.
/// [^footnote]: Footnotes aren’t parsed by default in swift-cmark, and swiftMarkup doesn’t
/// enable the feature.
- (void)f0;
@end


SWIFT_CLASS("_TtC8comments19HasThrowingFunction")
@interface HasThrowingFunction
/// Might throw something.
/// \param x A number
///
///
/// throws:
/// An error if <code>x == 0</code>
- (void)f1:(NSInteger)x;
@end


SWIFT_CLASS("_TtC8comments15HorizontalRules")
@interface HorizontalRules
/// Briefly.
/// <hr/>
/// The end.
- (void)f0;
@end


SWIFT_CLASS("_TtC8comments16ImplicitNameLink")
@interface ImplicitNameLink
/// <a href="https://www.apple.com/">Apple</a>
- (void)f0;
@end


SWIFT_CLASS("_TtC8comments20IndentedBlockComment")
@interface IndentedBlockComment
/// Brief.
/// First paragraph line.
/// Second paragraph line.
/// Now for a code sample:
/// \code
/// var x = 1
/// // var y = 2
/// var z = 3
///
/// \endcode
- (void)f1;
/// Hugely indented brief.
/// First paragraph line.
/// Second paragraph line.
/// Now for a code sample:
/// \code
/// var x = 1
/// // var y = 2
/// var z = 3
///
/// \endcode
- (void)f2;
@end


SWIFT_CLASS("_TtC8comments10InlineCode")
@interface InlineCode
/// Aaa <code>bbb</code> ccc.
- (void)f0;
@end


SWIFT_CLASS("_TtC8comments10InlineLink")
@interface InlineLink
/// Aaa <a href="/path/to/something">bbb</a> ccc.
- (void)f0;
@end


SWIFT_CLASS("_TtC8comments14MultiLineBrief")
@interface MultiLineBrief
/// Brief first line.
/// Brief after softbreak.
/// Some paragraph text.
- (void)f0;
@end


SWIFT_CLASS("_TtC8comments11OrderedList")
@interface OrderedList
/// <ol>
///   <li>
///     Aaa.
///   </li>
///   <li>
///     Bbb.
///     Ccc.
///   </li>
/// </ol>
- (void)f0;
@end


/// \param x A number
///
SWIFT_CLASS("_TtC8comments15ParamAndReturns")
@interface ParamAndReturns
/// Aaa.  f0.
/// \param first Bbb.
///
/// \param second Ccc.  Ddd.
/// Eee.
///
- (void)f0:(NSInteger)first second:(double)second;
/// Aaa.  f1.
/// \param first Bbb.
///
///
/// returns:
/// Ccc.
/// Ddd.
- (void)f1:(NSInteger)first;
/// Aaa.  f2.
/// \param first 
///
/// \param second Aaa.
///
/// \param third 
/// Bbb.
///
- (void)f2:(NSInteger)first second:(double)second third:(float)third;
/// Aaa.  f3.
/// \param first Bbb.
///
/// \param second Ccc.
///
/// \param third Ddd.
///
- (void)f3:(NSInteger)first second:(double)second third:(float)third;
/// Aaa.  f4.
///
/// returns:
/// Eee.
/// Fff.
- (void)f4;
@end


SWIFT_CLASS("_TtC8comments16ParameterOutline")
@interface ParameterOutline
/// \param x A number
///
/// \param y A number
///
/// \param z A number
///
- (void)f0:(NSInteger)x y:(NSInteger)y z:(NSInteger)z;
@end


SWIFT_CLASS("_TtC8comments22ParameterOutlineMiddle")
@interface ParameterOutlineMiddle
/// <ul>
///   <li>
///     This line should remain.
///   </li>
///   <li>
///     This line should also remain.
///   </li>
/// </ul>
/// \param x A number
///
/// \param y A number
///
/// \param z A number
///
- (void)f0:(NSInteger)x y:(NSInteger)y z:(NSInteger)z;
@end


SWIFT_CLASS("_TtC8comments13ReferenceLink")
@interface ReferenceLink
@end


SWIFT_CLASS("_TtC8comments7Returns")
@interface Returns
///
/// returns:
/// A number
- (NSInteger)f0 SWIFT_WARN_UNUSED_RESULT;
@end


SWIFT_CLASS("_TtC8comments18SeparateParameters")
@interface SeparateParameters
/// \param x A number
///
- (void)f0:(NSInteger)x y:(NSInteger)y;
@end


SWIFT_CLASS("_TtC8comments13SetextHeaders")
@interface SetextHeaders
/// <h1>LEVEL ONE</h1>
/// <h2>LEVEL TWO</h2>
/// <h3>LEVEL THREE</h3>
/// <h4>LEVEL FOUR</h4>
/// <h5>LEVEL FIVE</h5>
/// <h5>LEVEL SIX</h5>
- (void)f0;
@end


SWIFT_CLASS("_TtC8comments14StrongEmphasis")
@interface StrongEmphasis
/// Aaa <em>bbb</em> ccc.
/// Aaa <em>bbb</em> ccc.
- (void)f0;
@end


SWIFT_CLASS("_TtC8comments13UnorderedList")
@interface UnorderedList
/// <ul>
///   <li>
///     Aaa.
///   </li>
///   <li>
///     Bbb.
///     Ccc.
///   </li>
/// </ul>
/// <ul>
///   <li>
///     Ddd.
///   </li>
///   <li>
///     Eee.
///     <ul>
///       <li>
///         Fff.
///       </li>
///     </ul>
///   </li>
/// </ul>
- (void)f0;
@end

#endif
#if __has_attribute(external_source_symbol)
# pragma clang attribute pop
#endif
#if defined(__cplusplus)
#endif
#pragma clang diagnostic pop
#endif
