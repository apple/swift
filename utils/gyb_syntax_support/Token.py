from .Classification import classification_by_name
from .Node import error  # noqa: I201
from .kinds import lowercase_first_word  # noqa: I201


class Token(object):
    """
    Represents the specification for a Token in the TokenSyntax file.
    """

    def __init__(self, name, kind, serialization_code, unprefixed_kind=None,
                 text=None, classification='None', is_keyword=False,
                 requires_leading_space=False, requires_trailing_space=False):
        self.name = name
        self.kind = kind
        if unprefixed_kind is None:
            self.unprefixed_kind = kind
        else:
            self.unprefixed_kind = unprefixed_kind
        self.serialization_code = serialization_code
        self.text = text or ""
        self.classification = classification_by_name(classification)
        self.is_keyword = is_keyword
        self.requires_leading_space = requires_leading_space
        self.requires_trailing_space = requires_trailing_space

    def swift_kind(self):
        name = lowercase_first_word(self.name)
        if self.is_keyword:
            return name + 'Keyword'
        return name


class Keyword(Token):
    """
    Represents a keyword token.
    """

    def __init__(self, name, text, serialization_code,
                 classification='Keyword'):
        Token.__init__(self, name, 'kw_' + text, serialization_code,
                       unprefixed_kind=text, text=text,
                       classification=classification, is_keyword=True,
                       requires_trailing_space=True)

    def macro_name(self):
        return "KEYWORD"


class SwiftKeyword(Keyword):
    def macro_name(self):
        return "SWIFT_KEYWORD"


class DeclKeyword(SwiftKeyword):
    def macro_name(self):
        return "DECL_KEYWORD"


class StmtKeyword(SwiftKeyword):
    def macro_name(self):
        return "STMT_KEYWORD"


class ExprKeyword(SwiftKeyword):
    def macro_name(self):
        return "EXPR_KEYWORD"


class PatternKeyword(SwiftKeyword):
    def macro_name(self):
        return "PAT_KEYWORD"


class SilKeyword(Keyword):
    def macro_name(self):
        return "SIL_KEYWORD"


class PoundKeyword(Token):
    def __init__(self, name, kind, text, serialization_code,
                 classification='Keyword'):
        Token.__init__(self, name, 'pound_' + kind, serialization_code,
                       unprefixed_kind=kind, text=text,
                       classification=classification, is_keyword=True,
                       requires_trailing_space=True)

    def macro_name(self):
        return "POUND_KEYWORD"


class PoundObjectLiteral(PoundKeyword):
    def __init__(self, name, kind, text, serialization_code, description,
                 protocol, classification='ObjectLiteral'):
        PoundKeyword.__init__(self, name, kind, text, serialization_code,
                              classification)
        self.description = description
        self.protocol = protocol

    def macro_name(self):
        return "POUND_OBJECT_LITERAL"


class PoundConfig(PoundKeyword):
    def macro_name(self):
        return "POUND_CONFIG"


class PoundDirectiveKeyword(PoundKeyword):
    def __init__(self, name, kind, text, serialization_code,
                 classification='PoundDirectiveKeyword'):
        PoundKeyword.__init__(self, name, kind, text, serialization_code,
                              classification)

    def macro_name(self):
        return "POUND_DIRECTIVE_KEYWORD"


class PoundConditionalDirectiveKeyword(PoundDirectiveKeyword):
    def __init__(self, name, kind, text, serialization_code,
                 classification='PoundDirectiveKeyword'):
        PoundKeyword.__init__(self, name, kind, text, serialization_code,
                              classification)

    def macro_name(self):
        return "POUND_COND_DIRECTIVE_KEYWORD"


class Punctuator(Token):
    def macro_name(self):
        return "PUNCTUATOR"


class Literal(Token):
    def macro_name(self):
        return "LITERAL"


class Misc(Token):
    def macro_name(self):
        return "MISC"


SYNTAX_TOKENS = [
    # Keywords that start decls
    DeclKeyword('Associatedtype', 'associatedtype', serialization_code=1),
    DeclKeyword('Class', 'class', serialization_code=2),
    DeclKeyword('Deinit', 'deinit', serialization_code=3),
    DeclKeyword('Enum', 'enum', serialization_code=4),
    DeclKeyword('Extension', 'extension', serialization_code=5),
    DeclKeyword('Func', 'func', serialization_code=6),
    DeclKeyword('Import', 'import', serialization_code=7),
    DeclKeyword('Init', 'init', serialization_code=8),
    DeclKeyword('Inout', 'inout', serialization_code=9),
    DeclKeyword('Let', 'let', serialization_code=10),
    DeclKeyword('Operator', 'operator', serialization_code=11),
    DeclKeyword('Precedencegroup', 'precedencegroup', serialization_code=12),
    DeclKeyword('Protocol', 'protocol', serialization_code=13),
    DeclKeyword('Struct', 'struct', serialization_code=14),
    DeclKeyword('Subscript', 'subscript', serialization_code=15),
    DeclKeyword('Typealias', 'typealias', serialization_code=16),
    DeclKeyword('Var', 'var', serialization_code=17),

    DeclKeyword('Fileprivate', 'fileprivate', serialization_code=18),
    DeclKeyword('Internal', 'internal', serialization_code=19),
    DeclKeyword('Private', 'private', serialization_code=20),
    DeclKeyword('Public', 'public', serialization_code=21),
    DeclKeyword('Static', 'static', serialization_code=22),

    # Statement keywords
    StmtKeyword('Defer', 'defer', serialization_code=23),
    StmtKeyword('If', 'if', serialization_code=24),
    StmtKeyword('Guard', 'guard', serialization_code=25),
    StmtKeyword('Do', 'do', serialization_code=26),
    StmtKeyword('Repeat', 'repeat', serialization_code=27),
    StmtKeyword('Else', 'else', serialization_code=28),
    StmtKeyword('For', 'for', serialization_code=29),
    StmtKeyword('In', 'in', serialization_code=30),
    StmtKeyword('While', 'while', serialization_code=31),
    StmtKeyword('Return', 'return', serialization_code=32),
    StmtKeyword('Break', 'break', serialization_code=33),
    StmtKeyword('Continue', 'continue', serialization_code=34),
    StmtKeyword('Fallthrough', 'fallthrough', serialization_code=35),
    StmtKeyword('Switch', 'switch', serialization_code=36),
    StmtKeyword('Case', 'case', serialization_code=37),
    StmtKeyword('Default', 'default', serialization_code=38),
    StmtKeyword('Where', 'where', serialization_code=39),
    StmtKeyword('Catch', 'catch', serialization_code=40),
    StmtKeyword('Throw', 'throw', serialization_code=50),

    # Expression keywords
    ExprKeyword('As', 'as', serialization_code=41),
    ExprKeyword('Any', 'Any', serialization_code=42),
    ExprKeyword('False', 'false', serialization_code=43),
    ExprKeyword('Is', 'is', serialization_code=44),
    ExprKeyword('Nil', 'nil', serialization_code=45),
    ExprKeyword('Rethrows', 'rethrows', serialization_code=46),
    ExprKeyword('Super', 'super', serialization_code=47),
    ExprKeyword('Self', 'self', serialization_code=48),
    ExprKeyword('CapitalSelf', 'Self', serialization_code=49),
    ExprKeyword('True', 'true', serialization_code=51),
    ExprKeyword('Try', 'try', serialization_code=52),
    ExprKeyword('Throws', 'throws', serialization_code=53),

    Keyword('__FILE__', '__FILE__', serialization_code=54),
    Keyword('__LINE__', '__LINE__', serialization_code=55),
    Keyword('__COLUMN__', '__COLUMN__', serialization_code=56),
    Keyword('__FUNCTION__', '__FUNCTION__', serialization_code=57),
    Keyword('__DSO_HANDLE__', '__DSO_HANDLE__', serialization_code=58),

    # Pattern keywords
    PatternKeyword('Wildcard', '_', serialization_code=59),

    # Punctuators
    Punctuator('LeftParen', 'l_paren', text='(', serialization_code=88),
    Punctuator('RightParen', 'r_paren', text=')', serialization_code=89),
    Punctuator('LeftBrace', 'l_brace', text='{', serialization_code=90),
    Punctuator('RightBrace', 'r_brace', text='}', serialization_code=91),
    Punctuator('LeftSquareBracket', 'l_square', text='[',
               serialization_code=92),
    Punctuator('RightSquareBracket', 'r_square', text=']',
               serialization_code=93),
    Punctuator('LeftAngle', 'l_angle', text='<', requires_leading_space=True, 
               requires_trailing_space=True, serialization_code=94),
    Punctuator('RightAngle', 'r_angle', text='>', requires_leading_space=True,
               requires_trailing_space=True, serialization_code=95),

    Punctuator('Period', 'period', text='.', serialization_code=85),
    Punctuator('PrefixPeriod', 'period_prefix', text='.',
               serialization_code=87),
    Punctuator('Comma', 'comma', text=',', requires_trailing_space=True,
               serialization_code=84),
    Punctuator('Ellipsis', 'ellipsis', text='...', serialization_code=118),
    Punctuator('Colon', 'colon', text=':', requires_trailing_space=True,
               serialization_code=82),
    Punctuator('Semicolon', 'semi', text=';', serialization_code=83),
    Punctuator('Equal', 'equal', text='=', requires_leading_space=True,
               requires_trailing_space=True, serialization_code=86),
    Punctuator('AtSign', 'at_sign', text='@', classification='Attribute',
               serialization_code=80),
    Punctuator('Pound', 'pound', text='#', serialization_code=81),

    Punctuator('PrefixAmpersand', 'amp_prefix', text='&',
               requires_leading_space=True, requires_trailing_space=True, 
               serialization_code=96),
    Punctuator('Arrow', 'arrow', text='->', requires_trailing_space=True,
               serialization_code=78),


    Punctuator('Backtick', 'backtick', text='`', serialization_code=79),

    Punctuator('Backslash', 'backslash', text='\\\\', serialization_code=100),

    Punctuator('ExclamationMark', 'exclaim_postfix', text='!',
               serialization_code=99),

    Punctuator('PostfixQuestionMark', 'question_postfix', text='?',
               serialization_code=97),
    Punctuator('InfixQuestionMark', 'question_infix', text='?',
               serialization_code=98),

    Punctuator('StringQuote', 'string_quote', text='\\\"',
               classification='StringLiteral', serialization_code=102),
    Punctuator('SingleQuote', 'single_quote', text='\\\'',
               classification='StringLiteral', serialization_code=120),
    Punctuator('MultilineStringQuote', 'multiline_string_quote',
               text='\\\"\\\"\\\"', classification='StringLiteral',
               serialization_code=103),

    # Keywords prefixed with a '#'.

    PoundKeyword('PoundKeyPath', 'keyPath', text='#keyPath',
                 serialization_code=74),
    PoundKeyword('PoundLine', 'line', text='#line',
                 serialization_code=69),
    PoundKeyword('PoundSelector', 'selector', text='#selector',
                 serialization_code=73),
    PoundKeyword('PoundFile', 'file', text='#file',
                 serialization_code=68),
    PoundKeyword('PoundFileID', 'fileID', text='#fileID',
                 serialization_code=122),
    PoundKeyword('PoundFilePath', 'filePath', text='#filePath',
                 serialization_code=121),
    PoundKeyword('PoundColumn', 'column', text='#column',
                 serialization_code=70),
    PoundKeyword('PoundFunction', 'function', text='#function',
                 serialization_code=72),
    PoundKeyword('PoundDsohandle', 'dsohandle', text='#dsohandle',
                 serialization_code=71),
    PoundKeyword('PoundAssert', 'assert', text='#assert',
                 serialization_code=117),

    PoundDirectiveKeyword('PoundSourceLocation', 'sourceLocation',
                          text='#sourceLocation', serialization_code=65),
    PoundDirectiveKeyword('PoundWarning', 'warning', text='#warning',
                          serialization_code=66),
    PoundDirectiveKeyword('PoundError', 'error', text='#error',
                          serialization_code=67),

    PoundConditionalDirectiveKeyword('PoundIf', 'if', text='#if',
                                     serialization_code=64),
    PoundConditionalDirectiveKeyword('PoundElse', 'else', text='#else',
                                     serialization_code=62),
    PoundConditionalDirectiveKeyword('PoundElseif', 'elseif',
                                     text='#elseif', serialization_code=63),
    PoundConditionalDirectiveKeyword('PoundEndif', 'endif',
                                     text='#endif', serialization_code=61),

    PoundConfig('PoundAvailable', 'available', text='#available',
                serialization_code=60),
    PoundConfig('PoundUnavailable', 'unavailable', text='#unavailable',
                serialization_code=123),

    PoundObjectLiteral('PoundFileLiteral', 'fileLiteral',
                       text='#fileLiteral', serialization_code=76,
                       description='file reference',
                       protocol='ExpressibleByFileReferenceLiteral'),
    PoundObjectLiteral('PoundImageLiteral', 'imageLiteral',
                       text='#imageLiteral', serialization_code=77,
                       description='image',
                       protocol='ExpressibleByImageLiteral'),
    PoundObjectLiteral('PoundColorLiteral', 'colorLiteral',
                       text='#colorLiteral', serialization_code=75,
                       description='color',
                       protocol='ExpressibleByColorLiteral'),

    Literal('IntegerLiteral', 'integer_literal',
            classification='IntegerLiteral', serialization_code=111),
    Literal('FloatingLiteral', 'floating_literal',
            classification='FloatingLiteral', serialization_code=112),
    Literal('StringLiteral', 'string_literal',
            classification='StringLiteral', serialization_code=113),

    Misc('Unknown', 'unknown', serialization_code=115),
    Misc('Identifier', 'identifier', classification='Identifier',
         serialization_code=105),
    Misc('UnspacedBinaryOperator', 'oper_binary_unspaced',
         serialization_code=107),
    Misc('SpacedBinaryOperator', 'oper_binary_spaced', serialization_code=108),
    Misc('PostfixOperator', 'oper_postfix', serialization_code=110),
    Misc('PrefixOperator', 'oper_prefix', serialization_code=109),
    Misc('DollarIdentifier', 'dollarident', classification='DollarIdentifier',
         serialization_code=106),

    Misc('ContextualKeyword', 'contextual_keyword', classification='Keyword',
         serialization_code=114),
    Misc('RawStringDelimiter', 'raw_string_delimiter', serialization_code=119),
    Misc('StringSegment', 'string_segment', classification='StringLiteral',
         serialization_code=104),
    Misc('StringInterpolationAnchor', 'string_interpolation_anchor',
         text=')', classification='StringInterpolationAnchor',
         serialization_code=101),
    Misc('Yield', 'kw_yield', serialization_code=116, text='yield'),

]

SYNTAX_TOKEN_MAP = {token.name + 'Token': token for token in SYNTAX_TOKENS}


def verify_no_duplicate_serialization_codes(tokens):
    used_codes = set()
    for token in tokens:
        if token.serialization_code in used_codes:
            error("Serialization code %d used twice for tokens" %
                  token.serialization_code)
        used_codes.add(token.serialization_code)


verify_no_duplicate_serialization_codes(SYNTAX_TOKENS)
