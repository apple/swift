import os
import re
import sys
import ahocorasick

def extract_diagnostics_from_file(file_path):
    """
    Extract diagnostics patterns from a given file.

    Args:
    - file_path (str): Path to the file.

    Returns:
    - list: List of extracted diagnostics with the "diag::" prefix.
    """
    with open(file_path, 'r') as file:
        content = file.read()
        pattern = r'^\s*(WARNING|NOTE|ERROR|REMARK)\(([^,]+),'
        return ["diag::" + match.group(2).strip() for match in re.finditer(pattern, content, re.MULTILINE)]

def extract_all_diagnostics():
    """
    Recursively extract diagnostics from all diagnostics definition files.

    Returns:
    - list: List of all extracted diagnostics.
    """
    file_pattern = re.compile(r'diagnostic.*\.def$', re.IGNORECASE)
    identifiers = []

    for dirpath, dirnames, filenames in os.walk('.'):
        for filename in filenames:
            if file_pattern.match(filename):
                file_path = os.path.join(dirpath, filename)
                identifiers.extend(extract_diagnostics_from_file(file_path))

    return identifiers

def build_automaton(patterns):
    """
    Build an Aho-Corasick automaton from the given patterns.

    Args:
    - patterns (list): List of patterns.

    Returns:
    - Automaton: Aho-Corasick automaton.
    """
    A = ahocorasick.Automaton()
    for idx, pattern in enumerate(patterns):
        A.add_word(pattern, (idx, pattern))
    A.make_automaton()
    return A

def check_strings_in_files(strings, folder_path):
    """
    Check if all strings appear in the files within the given folder (including subfolders).

    Args:
    - strings (list): List of strings to search for.
    - folder_path (str): Path to the folder.

    Returns:
    - list: List of strings that haven't been found.
    """
    pattern_map = {s: False for s in strings}
    automaton = build_automaton(strings)

    for dirpath, dirnames, filenames in os.walk(folder_path):
        if all(pattern_map.values()):  # End early if all patterns are found
            break
        for file_name in filenames:
            file_path = os.path.join(dirpath, file_name)
            with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
                for _, (_, found) in automaton.iter(f.read()):
                    pattern_map[found] = True

    return [key for key, found in pattern_map.items() if not found]

def check_for_unused_diagnostics():
    diagnostics = extract_all_diagnostics()
    unused_diagnostics = check_strings_in_files(diagnostics, './lib')

    if not unused_diagnostics:
        print("All diagnostics appear at least once in the codebase!")
        return 0
    else:
        print("The following diagnostics did not appear in the codebase:")
        for diag in unused_diagnostics:
            print(diag)
        return 1

if __name__ == '__main__':
    sys.exit(check_for_unused_diagnostics())
