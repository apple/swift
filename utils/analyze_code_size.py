#!/usr/bin/env python3

import argparse
import re
import subprocess
import sys


useCSV = False
groupSpecializations = False
listGroupSpecializations = False


def main(arguments):
    parser = argparse.ArgumentParser(
        description='Analyze the code size in a binary')
    parser.add_argument('-arch', type=str,
                        help='the arch to look at', default='arm64')
    parser.add_argument('-categorize', action='store_true',
                        help='categorize symbols', dest='build_categories',
                        default=False)
    parser.add_argument('-list-category', type=str,
                        help='list symbols in category')
    parser.add_argument('-group-specializations', action='store_true',
                        help='group specializations')
    parser.add_argument('-list-group-specializations', action='store_true',
                        help='list group specializations')
    parser.add_argument('-csv', dest='use_csv', action='store_true',
                        help='print results as csv')
    parser.add_argument('-uncategorized', action='store_true',
                        help='show all uncategorized symbols',
                        dest='show_uncategorized',
                        default=False)
    parser.add_argument('bin', help='the binary')
    parser.set_defaults(use_csv=False)

    args = parser.parse_args(arguments)
    if args.use_csv:
        global useCSV
        useCSV = True
        print("Using csv")

    if args.group_specializations:
        global groupSpecializations
        groupSpecializations = True

    if args.list_group_specializations:
        global listGroupSpecializations
        listGroupSpecializations = True

    segments = parse_segments(args.bin, args.arch)

    if args.build_categories:
        categorize(segments)
    elif args.show_uncategorized:
        uncategorized(segments)
    elif args.list_category:
        list_category(segments, args.list_category)
    else:
        show_all(segments)


class Symbol(object):
    def __init__(self, name, mangled_name, size):
        self.name = name
        self.mangled_name = mangled_name
        self.count = 1
        self.size = int(size)


def get_symbol_size(sym):
    return sym.size


class Segment(object):
    def __init__(self, name):
        self.name = name
        self.sections = []


class Section(object):
    def __init__(self, name, size):
        self.name = name
        self.size = size
        self.symbols = []


class Category(object):
    def __init__(self, name):
        self.name = name
        self.size = 0
        self.symbols = []

    def add(self, symbol):
        self.symbols.append(symbol)
        self.size += symbol.size


class GenericSpecializationGroupKey(object):
    def __init__(self, module_name, type_name, specialization):
        self.module_name = module_name
        self.type_name = type_name
        self.specialization = specialization

    def __hash__(self):
        return hash((self.module_name, self.type_name, self.specialization))

    def __eq__(self, other):
        return (self.module_name == other.module_name
                and self.type_name == other.type_name
                and self.specialization == other.specialization)

    def __ne__(self, other):
        return not self.__eq__(other)


class GenericSpecialization(object):
    def __init__(self, module_name, type_name, specialization):
        self.module_name = module_name
        self.type_name = type_name
        self.specialization = specialization
        self.size = 0
        self.symbols = []

    def add(self, symbol):
        self.symbols.append(symbol)
        self.size += symbol.size

    def list_symbols(self):
        sorted_symbols = []
        for symbol in self.symbols:
            sorted_symbols.append((symbol.name, symbol.size))
        sorted_symbols.sort(key=lambda entry: entry[1], reverse=True)
        for symbol in sorted_symbols:
            print("%9d %s" % (symbol[1], symbol[0]))


class Categories(object):
    def __init__(self):
        self.category_matching = [
            ['Objective-C function', re.compile(r'.*[+-]\[')],
            ['C++', re.compile(r'_+swift')],
            ['Generic specialization of stdlib',
                re.compile(
                    r'.*generic specialization.* of ' +
                    r'(static )?(\(extension in Swift\):)?Swift\.'
                )],
            ['Generic specialization',
                re.compile(r'.*generic specialization')],
            ['Merged function', re.compile(r'merged ')],
            ['Key path', re.compile(r'key path')],
            ['Function signature specialization',
                re.compile(r'function signature specialization')],
            ['Reabstraction thunk helper',
                re.compile(r'reabstraction thunk helper')],
            ['vtable thunk', re.compile(r'vtable thunk for')],
            ['@objc thunk', re.compile(r'@objc')],
            ['@nonobjc thunk', re.compile(r'@nonobjc')],
            ['Value witness', re.compile(r'.*value witness for')],
            ['Type layout string', re.compile(r'.*type_layout_string')],
            ['Block copy helper', re.compile(r'_block_copy_helper')],
            ['Block destroy helper', re.compile(r'_block_destroy_helper')],
            ['Block literal global', re.compile(r'___block_literal_global')],
            ['Destroy helper block', re.compile(r'___destroy_helper_block')],
            ['Copy helper block', re.compile(r'___copy_helper_block')],
            ['Object destroy', re.compile(r'_objectdestroy')],
            ['Partial apply forwarder',
                re.compile(r'partial apply forwarder')],
            ['Closure function', re.compile(r'closure #')],
            ['ObjC metadata update function',
                re.compile(r'ObjC metadata update function for')],
            ['Variable initialization expression',
                re.compile(r'variable initialization expression of')],
            ['Global initialization', re.compile(r'_globalinit_')],
            ['Unnamed', re.compile(r'___unnamed_')],
            ['Dyld stubs', re.compile(r'DYLD-STUB\$')],
            ['Witness table accessor',
                re.compile(r'.*witness table accessor for')],
            ['Protocol witness', re.compile(r'protocol witness for')],
            ['Outlined variable', re.compile(r'outlined variable #')],
            ['Outlined value function (copy,destroy,release...)',
                re.compile(r'outlined')],
            ['_symbolic', re.compile(r'_symbolic')],
            ['_associated conformance',
                re.compile(r'_associated conformance')],
            ['Direct field offset', re.compile(r'direct field offset for')],
            ['Value witness tables', re.compile(r'.*value witness table')],
            ['Protocol witness table',
                re.compile(r'.*protocol witness table for')],
            ['Protocol conformance descriptor',
                re.compile(r'protocol conformance descriptor for')],
            ['Lazy protocol witness table cache var',
                re.compile(
                    r'lazy protocol witness table cache variable for type')],
            ['Nominal type descriptor',
                re.compile(r'nominal type descriptor for')],
            ['ObjC class', re.compile(r'_OBJC_CLASS_')],
            ['ObjC metaclass', re.compile(r'_OBJC_METACLASS')],
            ['ObjC ivar', re.compile(r'_OBJC_IVAR')],
            ['Metaclass', re.compile(r'metaclass for')],
            ['Block descriptor', re.compile(r'_+block_descriptor')],
            ['Extension descriptor', re.compile(r'extension descriptor')],
            ['Module descriptor', re.compile(r'module descriptor')],
            ['Associated type descriptor',
                re.compile(r'associated type descriptor for')],
            ['Associated conformance descriptor',
                re.compile(r'associated conformance descriptor for')],
            ['Protocol descriptor', re.compile(r'protocol descriptor for')],
            ['Base conformance descriptor',
                re.compile(r'base conformance descriptor for')],
            ['Protocol requirements base descriptor',
                re.compile(r'protocol requirements base descriptor for')],
            ['Property descriptor', re.compile(r'property descriptor for')],
            ['Method descriptor', re.compile(r'method descriptor for')],
            ['Anonymous descriptor', re.compile(r'anonymous descriptor')],
            ['Type metadata accessor',
                re.compile(r'.*type metadata accessor')],
            ['Type metadata', re.compile(r'.*type metadata')],
            ['Reflection metadata descriptor',
                re.compile(r'reflection metadata .* descriptor')],
        ]

        self.category_mangled_matching = [
            ['Swift variable storage', re.compile(r'^_\$s.*[v][p][Z]?$')],
            ['Swift constructor', re.compile(r'^_\$s.*[f][cC]$')],
            ['Swift initializer', re.compile(r'^_\$s.*[f][ie]$')],
            ['Swift destructor/destroyer', re.compile(r'^_\$s.*[f][dDE]$')],
            ['Swift getter', re.compile(r'^_\$s.*[iv][gG]$')],
            ['Swift setter', re.compile(r'^_\$s.*[iv][swW]$')],
            ['Swift materializeForSet', re.compile(r'^_\$s.*[iv][m]$')],
            ['Swift modify', re.compile(r'^_\$s.*[iv][M]$')],
            ['Swift read', re.compile(r'^_\$s.*[iv][r]$')],
            ['Swift addressor', re.compile(r'^_\$s.*[iv][al][uOop]$')],
            ['Swift function', re.compile(r'^_\$s.*F$')],
            ['Swift unknown', re.compile(r'^_\$s.*')],
        ]
        self.categories = {}
        self.specializations = {}
        self.specialization_matcher = re.compile(
            r'.*generic specialization <(?P<spec_list>.*)> of' +
            r' (static )?(\(extension in Swift\):)?(?P<module_name>[^.]*)\.' +
            r'(?:(?P<first_type>[^.^(^<]*)\.){0,1}' +
            r'(?:(?P<last_type>[^.^(^<]*)\.)*(?P<function_name>[^(^<]*)'
        )
        self.single_stdlib_specialized_type_matcher = re.compile(
            r'(Swift\.)?[^,^.]*$'
        )
        self.two_specialized_stdlib_types_matcher = re.compile(
            r'(Swift\.)?[^,^.]*, (Swift\.)?[^,^.]*$'
        )
        self.single_specialized_foundation_type_matcher = re.compile(
            r'(Foundation\.)?[^,^.]*$'
        )
        self.two_specialized_foundation_types_matcher = re.compile(
            r'(Swift\.)?[^,^.]*, (Foundation\.)?[^,^.]*$'
        )
        self.two_specialized_foundation_types_matcher2 = re.compile(
            r'(Foundation\.)?[^,^.]*, (Foundation\.)?[^,^.]*$'
        )
        self.two_specialized_foundation_types_matcher3 = re.compile(
            r'(Foundation\.)?[^,^.]*, (Swift\.)?[^,^.]*$'
        )
        self.array_type_matcher = re.compile(r'Array')
        self.dictionary = re.compile(r'Array')
        self.single_specialized_types_matcher = re.compile(
            r'(?P<module_name>[^,^.]*)\.([^,^.]*\.)*(?P<type_name>[^,^.]*)$'
        )
        self.is_class_type_dict = {}
        self.stdlib_and_other_type_matcher = re.compile(
            r'(Swift\.)?[^,^.]*, (?P<module_name>[^,^.]*)\.(?P<type_name>[^,^.]*)$'
        )
        self.foundation_and_other_type_matcher = re.compile(
            r'(Foundation\.)?[^,^.]*, (?P<module_name>[^,^.]*)\.' +
            r'(?P<type_name>[^,^.]*)$'
        )

    def categorize_by_name(self, symbol):
        for c in self.category_matching:
            if c[1].match(symbol.name):
                return c[0]
        return None

    def categorize_by_mangled_name(self, symbol):
        for c in self.category_mangled_matching:
            if c[1].match(symbol.mangled_name):
                return c[0]
        return None

    def add_symbol(self, category_name, symbol):
        existing_category = self.categories.get(category_name)
        if existing_category:
            existing_category.add(symbol)
        else:
            new_category = Category(category_name)
            new_category.add(symbol)
            self.categories[category_name] = new_category

    def add(self, symbol):
        category_name = self.categorize_by_name(symbol)
        if category_name:
            self.add_symbol(category_name, symbol)
            if (groupSpecializations and
                    category_name == 'Generic specialization of stdlib'):
                self.add_specialization(symbol)
            return
        category_name = self.categorize_by_mangled_name(symbol)
        if category_name:
            self.add_symbol(category_name, symbol)
        else:
            self.add_symbol('Unknown', symbol)
        if (groupSpecializations and
                category_name == 'Generic specialization of stdlib'):
            self.add_specialization(symbol)

    def is_class_type_(self, type_name, mangled_name):
        match_class_name = str(len(type_name)) + type_name + 'C'
        if match_class_name in mangled_name:
            return True
        return False

    def is_class_type(self, type_name, mangled_name):
        existing_categorization = self.is_class_type_dict.get(type_name, 3)
        if existing_categorization == 3:
            is_class = self.is_class_type_(type_name, mangled_name)
            self.is_class_type_dict[type_name] = is_class
            return is_class
        else:
            return existing_categorization

    def is_dictionary_like_type(self, type_name):
        if 'Dictionary' in type_name:
            return True
        if 'Set' in type_name:
            return True
        return False

    def group_library_types(self, module, type_name, specialization, mangled_name):
        if module != 'Swift':
            return module, type_name, specialization
        if self.single_stdlib_specialized_type_matcher.match(specialization):
            return module, 'stdlib', 'stdlib'
        if self.two_specialized_stdlib_types_matcher.match(specialization):
            return module, 'stdlib', 'stdlib'
        if self.single_specialized_foundation_type_matcher.match(specialization):
            return module, 'stdlib', 'foundation'
        if self.two_specialized_foundation_types_matcher.match(specialization):
            return module, 'stdlib', 'foundation'
        if self.two_specialized_foundation_types_matcher2.match(specialization):
            return module, 'stdlib', 'foundation'
        if self.two_specialized_foundation_types_matcher3.match(specialization):
            return module, 'stdlib', 'foundation'
        single_spec = self.single_specialized_types_matcher.match(specialization)
        if single_spec:
            is_class = self.is_class_type(single_spec.group('type_name'), mangled_name)
            is_dict = type_name is not None and self.is_dictionary_like_type(type_name)
            if not is_dict and is_class:
                return module, 'stdlib', 'class'
            if is_dict and is_class:
                return module, 'stdlib', 'class(dict)'
        stdlib_other_spec = self.stdlib_and_other_type_matcher.match(specialization)
        if stdlib_other_spec:
            is_class = self.is_class_type(stdlib_other_spec.group('type_name'),
                                          mangled_name)
            if is_class:
                return module, 'stdlib', 'stdlib, class'
        foundation_other_spec = self.foundation_and_other_type_matcher.match(
            specialization)
        if foundation_other_spec:
            is_class = self.is_class_type(foundation_other_spec.group('type_name'),
                                          mangled_name)
            if is_class:
                return module, 'stdlib', 'foundation, class'
        return module, 'stdlib', 'other'

    def add_specialization(self, symbol):
        specialization_match = self.specialization_matcher.match(symbol.name)
        if specialization_match:
            module = specialization_match.group('module_name')
            type_name = specialization_match.group('first_type')
            specialization = specialization_match.group('spec_list')
            module, type_name, specialization = self.group_library_types(
                module, type_name, specialization, symbol.mangled_name)
            key = GenericSpecializationGroupKey(module, type_name, specialization)
            existing_specialization = self.specializations.get(key)
            if existing_specialization:
                existing_specialization.add(symbol)
            else:
                new_specialization = GenericSpecialization(module, type_name,
                                                           specialization)
                new_specialization.add(symbol)
                self.specializations[key] = new_specialization
        else:
            print(symbol.name)
            print('not matched')
        return

    def print_specializations(self):
        values = self.specializations.values()
        sorted_specializations = []
        for v in values:
            sorted_specializations.append(v)

        if not sorted_specializations:
            return None
        else:
            sorted_specializations.sort(key=lambda entry: entry.specialization)
            sorted_specializations.sort(key=lambda entry: entry.type_name)
            sorted_specializations.sort(key=lambda entry: entry.module_name)
            print("Specialization info")
            for spec in sorted_specializations:
                print("%20s.%s %20s %8d" % (spec.module_name, spec.type_name,
                                            spec.specialization, spec.size))
                if listGroupSpecializations:
                    spec.list_symbols()
            print("")
            return None

    def categorize(self, symbols):
        for sym in symbols:
            self.add(sym)

    def print_summary(self, section_size):
        names = [c[0] for c in self.category_matching]
        names.extend([c[0] for c in self.category_mangled_matching])
        names.append('Unknown')
        total_size = 0
        sorted_categories = []
        for name in names:
            category = self.categories.get(name)
            size = 0
            if category:
                size = category.size
            total_size += size
            if size > 0:
                sorted_categories.append(
                    (name, size, (float(size) * 100) / section_size))
        sorted_categories.sort(key=lambda entry: entry[1], reverse=True)
        for category in sorted_categories:
            if useCSV:
                print("%s;%d;%.2f%%" %
                      (category[0], category[1], category[2]))
            else:
                print("%60s: %8d (%6.2f%%)" %
                      (category[0], category[1], category[2]))
        print("%60s: %8d (%6.2f%%)" % ('TOTAL', total_size, float(100)))

    def uncategorizedSymbols(self):
        category = self.categories.get('Unknown')
        if category:
            return category.symbols
        return None

    def print_uncategorizedSymbols(self):
        syms = self.uncategorizedSymbols()
        if syms:
            for symbol in syms:
                print(symbol.mangled_name + " " + symbol.name + " " +
                      str(symbol.size))

    def print_category(self, category):
        category = self.categories.get(category)
        if category:
            if category.symbols:
                sorted_symbols = sorted(category.symbols, key=get_symbol_size)
                for sym in sorted_symbols:
                    print('%8d %s %s' % (sym.size, sym.name, sym.mangled_name))

    def has_category(self, category):
        category = self.categories.get(category)
        if category:
            if category.symbols:
                return True
        return False


def parse_segments(path, arch):
    mangled = subprocess.check_output(
        ['symbols', '-noSources', '-noDemangling', '-arch', arch, path])
    demangle = subprocess.Popen(
        ['xcrun', 'swift-demangle'], stdin=subprocess.PIPE,
        stdout=subprocess.PIPE)
    demangled = demangle.communicate(mangled)[0].decode('utf-8')
    symbols = {}
    segments = []
    segment_regex = re.compile(
        r"^        0x[0-9a-f]+ \(\s*0x(?P<size>[0-9a-f]+)\) "
        r"(?P<name>.+?) (?P<name2>.+?)$")
    object_file_segment_regex = re.compile(
        r"^        0x[0-9a-f]+ \(\s*0x(?P<size>[0-9a-f]+)\)  "
        r"SEGMENT$")
    section_regex = re.compile(
        r"^            0x[0-9a-f]+ \(\s*0x(?P<size>[0-9a-f]+)\) "
        r"(?P<name>.+?) (?P<name2>.+?)$")
    symbol_regex = re.compile(
        r"^                0x[0-9a-f]+ \(\s*0x(?P<size>[0-9a-f]+)\) "
        r"(?P<name>.+?) \[[^\]]+\] $")

    mangled_lines = mangled.decode('utf-8').splitlines()
    current_line_number = 0

    for line in demangled.splitlines():
        mangled_line = mangled_lines[current_line_number]
        current_line_number += 1

        # Match a segment entry.
        segment_match = segment_regex.match(line)
        if segment_match:
            new_segment = Segment(segment_match.group('name'))
            segments.append(new_segment)
            continue

        object_file_segment_match = object_file_segment_regex.match(line)
        if object_file_segment_match:
            new_segment = Segment("SEGMENT")
            segments.append(new_segment)
            continue

        # Match a section entry.
        section_match = section_regex.match(line)
        if section_match:
            new_section = Section(section_match.group('name2'),
                                  int(section_match.group('size'), 16))
            segments[-1].sections.append(new_section)
            continue

        # Match a symbol entry.
        symbol_match = symbol_regex.match(line)
        if not symbol_match:
            continue
        mangled_symbol_match = symbol_regex.match(mangled_line)
        if not mangled_symbol_match:
            print('mangled and demangled mismatch')
            print(mangled_line)
            print(line)
            assert False

        symbol = Symbol(symbol_match.group('name'),
                        mangled_symbol_match.group('name'),
                        int(symbol_match.group('size'), 16))
        existing = symbols.get(symbol.name)
        if existing:
            existing.size += symbol.size
        else:
            symbols[symbol.name] = symbol
            segments[-1].sections[-1].symbols.append(symbol)

    return segments


def show_all(segments):
    for segment in segments:
        for section in segment.sections:
            symbols = section.symbols
            for sym in symbols:
                print(str(sym.size) + ' ' + sym.name + ' ' + sym.mangled_name)


def categorize(segments):
    for segment in segments:
        for section in segment.sections:
            print('Section %52s: %8d' %
                  (segment.name + ';' + section.name, section.size))
            symbols = section.symbols
            categories = Categories()
            categories.categorize(symbols)
            categories.print_summary(section.size)
            print('')
            if groupSpecializations:
                categories.print_specializations()


def uncategorized(segments):
    for segment in segments:
        for section in segment.sections:
            symbols = section.symbols
            categories = Categories()
            categories.categorize(symbols)
            categories.print_uncategorizedSymbols()


def list_category(segments, category):
    for segment in segments:
        for section in segment.sections:
            symbols = section.symbols
            categories = Categories()
            categories.categorize(symbols)
            if categories.has_category(category):
                print('Section %22s: %8d' %
                      (segment.name + ';' + section.name, section.size))
                categories.print_category(category)
                print('')
                if groupSpecializations:
                    categories.print_specializations()


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
