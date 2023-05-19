class Token(object):
    """
    Represents the specification for a Token in the TokenSyntax file.
    """

    def __init__(self, name, kind, unprefixed_kind=None, text=None):
        self.name = name
        if unprefixed_kind is None:
            self.unprefixed_kind = kind
        else:
            self.unprefixed_kind = unprefixed_kind
        self.text = text


class Keyword(Token):
    """
    Represents a keyword token.
    """

    def __init__(self, name, text):
        Token.__init__(self, name=name, kind='kw_' + text, unprefixed_kind=text,
                       text=text)

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
    def __init__(self, name, kind, text, name_for_diagnostics=None):
        if name_for_diagnostics is None:
            name_for_diagnostics = text
        Token.__init__(self, name=name, kind='pound_' + kind, unprefixed_kind=kind,
                       text=text)

    def macro_name(self):
        return "POUND_KEYWORD"


class PoundObjectLiteral(PoundKeyword):
    def __init__(self, name, kind, text, name_for_diagnostics,
                 protocol):
        PoundKeyword.__init__(
            self,
            name=name,
            kind=kind,
            text=text,
            name_for_diagnostics=name_for_diagnostics
        )
        self.description = name_for_diagnostics
        self.protocol = protocol

    def macro_name(self):
        return "POUND_OBJECT_LITERAL"


class PoundConfig(PoundKeyword):
    def macro_name(self):
        return "POUND_CONFIG"


class PoundDirectiveKeyword(PoundKeyword):
    def __init__(self, name, kind, text):
        PoundKeyword.__init__(
            self,
            name=name,
            kind=kind,
            text=text
        )

    def macro_name(self):
        return "POUND_DIRECTIVE_KEYWORD"


class PoundConditionalDirectiveKeyword(PoundDirectiveKeyword):
    def __init__(self, name, kind, text):
        PoundKeyword.__init__(
            self,
            name=name,
            kind=kind,
            text=text
        )

    def macro_name(self):
        return "POUND_COND_DIRECTIVE_KEYWORD"


class Punctuator(Token):
    def __init__(self, name, kind, text):
        Token.__init__(
            self,
            name=name,
            kind=kind,
            unprefixed_kind=None,
            text=text
        )

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
    DeclKeyword('Associatedtype', 'associatedtype'),
    DeclKeyword('Class', 'class'),
    DeclKeyword('Deinit', 'deinit'),
    DeclKeyword('Enum', 'enum'),
    DeclKeyword('Extension', 'extension'),
    DeclKeyword('Func', 'func'),
    DeclKeyword('Import', 'import'),
    DeclKeyword('Init', 'init'),
    DeclKeyword('Inout', 'inout'),
    DeclKeyword('Let', 'let'),
    DeclKeyword('Operator', 'operator'),
    DeclKeyword('Precedencegroup', 'precedencegroup'),
    DeclKeyword('Protocol', 'protocol'),
    DeclKeyword('Struct', 'struct'),
    DeclKeyword('Subscript', 'subscript'),
    DeclKeyword('Typealias', 'typealias'),
    DeclKeyword('Var', 'var'),

    DeclKeyword('Fileprivate', 'fileprivate'),
    DeclKeyword('Internal', 'internal'),
    DeclKeyword('Private', 'private'),
    DeclKeyword('Public', 'public'),
    DeclKeyword('Static', 'static'),

    # Statement keywords
    StmtKeyword('Defer', 'defer'),
    StmtKeyword('If', 'if'),
    StmtKeyword('Guard', 'guard'),
    StmtKeyword('Do', 'do'),
    StmtKeyword('Repeat', 'repeat'),
    StmtKeyword('Else', 'else'),
    StmtKeyword('For', 'for'),
    StmtKeyword('In', 'in'),
    StmtKeyword('While', 'while'),
    StmtKeyword('Return', 'return'),
    StmtKeyword('Break', 'break'),
    StmtKeyword('Continue', 'continue'),
    StmtKeyword('Fallthrough', 'fallthrough'),
    StmtKeyword('Switch', 'switch'),
    StmtKeyword('Case', 'case'),
    StmtKeyword('Default', 'default'),
    StmtKeyword('Where', 'where'),
    StmtKeyword('Catch', 'catch'),
    StmtKeyword('Throw', 'throw'),

    # Expression keywords
    ExprKeyword('As', 'as'),
    ExprKeyword('Any', 'Any'),
    ExprKeyword('False', 'false'),
    ExprKeyword('Is', 'is'),
    ExprKeyword('Nil', 'nil'),
    ExprKeyword('Rethrows', 'rethrows'),
    ExprKeyword('Super', 'super'),
    ExprKeyword('Self', 'self'),
    ExprKeyword('CapitalSelf', 'Self'),
    ExprKeyword('True', 'true'),
    ExprKeyword('Try', 'try'),
    ExprKeyword('Throws', 'throws'),

    # Pattern keywords
    PatternKeyword('Wildcard', '_'),

    # Punctuators
    Punctuator('LeftParen', 'l_paren', text='('),
    Punctuator('RightParen', 'r_paren', text=')'),
    Punctuator('LeftBrace', 'l_brace', text='{'),
    Punctuator('RightBrace', 'r_brace', text='}'),
    Punctuator('LeftSquareBracket', 'l_square', text='['),
    Punctuator('RightSquareBracket', 'r_square', text=']'),
    Punctuator('LeftAngle', 'l_angle', text='<'),
    Punctuator('RightAngle', 'r_angle', text='>'),

    Punctuator('Period', 'period', text='.'),
    Punctuator('PrefixPeriod', 'period_prefix', text='.'),
    Punctuator('Comma', 'comma', text=','),
    Punctuator('Ellipsis', 'ellipsis', text='...'),
    Punctuator('Colon', 'colon', text=':'),
    Punctuator('Semicolon', 'semi', text=';'),
    Punctuator('Equal', 'equal', text='='),
    Punctuator('AtSign', 'at_sign', text='@'),
    Punctuator('Pound', 'pound', text='#'),

    Punctuator('PrefixAmpersand', 'amp_prefix', text='&'),
    Punctuator('Arrow', 'arrow', text='->'),

    Punctuator('Backtick', 'backtick', text='`'),

    Punctuator('Backslash', 'backslash', text='\\\\'),

    Punctuator('ExclamationMark', 'exclaim_postfix', text='!'),

    Punctuator('PostfixQuestionMark', 'question_postfix', text='?'),
    Punctuator('InfixQuestionMark', 'question_infix', text='?'),

    Punctuator('StringQuote', 'string_quote', text='\\\"'),
    Punctuator('SingleQuote', 'single_quote', text='\\\''),
    Punctuator('MultilineStringQuote', 'multiline_string_quote',
               text='\\\"\\\"\\\"'),

    # Keywords prefixed with a '#'.

    PoundKeyword('PoundKeyPath', 'keyPath', text='#keyPath'),
    PoundKeyword('PoundLine', 'line', text='#line'),
    PoundKeyword('PoundSelector', 'selector', text='#selector'),
    PoundKeyword('PoundFile', 'file', text='#file'),
    PoundKeyword('PoundFileID', 'fileID', text='#fileID'),
    PoundKeyword('PoundFilePath', 'filePath', text='#filePath'),
    PoundKeyword('PoundColumn', 'column', text='#column'),
    PoundKeyword('PoundFunction', 'function', text='#function'),
    PoundKeyword('PoundDsohandle', 'dsohandle', text='#dsohandle'),
    PoundKeyword('PoundAssert', 'assert', text='#assert'),

    PoundDirectiveKeyword('PoundSourceLocation', 'sourceLocation',
                          text='#sourceLocation'),
    PoundDirectiveKeyword('PoundWarning', 'warning', text='#warning'),
    PoundDirectiveKeyword('PoundError', 'error', text='#error'),

    PoundConditionalDirectiveKeyword('PoundIf', 'if', text='#if'),
    PoundConditionalDirectiveKeyword('PoundElse', 'else', text='#else'),
    PoundConditionalDirectiveKeyword('PoundElseif', 'elseif',
                                     text='#elseif'),
    PoundConditionalDirectiveKeyword('PoundEndif', 'endif',
                                     text='#endif'),

    PoundConfig('PoundAvailable', 'available', text='#available'),
    PoundConfig('PoundUnavailable', 'unavailable', text='#unavailable'),

    PoundObjectLiteral('PoundFileLiteral', 'fileLiteral',
                       text='#fileLiteral',
                       name_for_diagnostics='file reference',
                       protocol='ExpressibleByFileReferenceLiteral'),
    PoundObjectLiteral('PoundImageLiteral', 'imageLiteral',
                       text='#imageLiteral',
                       name_for_diagnostics='image',
                       protocol='ExpressibleByImageLiteral'),
    PoundObjectLiteral('PoundColorLiteral', 'colorLiteral',
                       text='#colorLiteral',
                       name_for_diagnostics='color',
                       protocol='ExpressibleByColorLiteral'),

    PoundConfig('PoundHasSymbol', '_hasSymbol', text='#_hasSymbol'),

    Literal('IntegerLiteral', 'integer_literal'),
    Literal('FloatingLiteral', 'floating_literal'),
    Literal('StringLiteral', 'string_literal'),
    Literal('RegexLiteral', 'regex_literal'),

    Misc('Unknown', 'unknown'),
    Misc('Identifier', 'identifier'),
    Misc('UnspacedBinaryOperator', 'oper_binary_unspaced'),
    Misc('SpacedBinaryOperator', 'oper_binary_spaced'),
    Misc('PostfixOperator', 'oper_postfix'),
    Misc('PrefixOperator', 'oper_prefix'),
    Misc('DollarIdentifier', 'dollarident'),

    Misc('ContextualKeyword', 'contextual_keyword'),
    Misc('RawStringDelimiter', 'raw_string_delimiter'),
    Misc('StringSegment', 'string_segment'),
    Misc('StringInterpolationAnchor', 'string_interpolation_anchor', text=')'),
    Misc('Yield', 'kw_yield', text='yield'),
    Misc('Forget', 'kw_forget', text='_forget'),  # NOTE: support for deprecated _forget
    Misc('Discard', 'kw_discard', text='discard'),

]

SYNTAX_TOKEN_MAP = {token.name + 'Token': token for token in SYNTAX_TOKENS}
