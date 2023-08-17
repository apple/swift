#ifndef TEST_INTEROP_CXX_STDLIB_INPUTS_STD_MAP_H
#define TEST_INTEROP_CXX_STDLIB_INPUTS_STD_MAP_H

#include <map>
#include <unordered_map>
#include <string>

using Map = std::map<int, int>;
using MapStrings = std::map<std::string, std::string>;
using NestedMap = std::map<int, Map>;
using UnorderedMap = std::unordered_map<int, int>;

inline Map initMap() { return {{1, 3}, {2, 2}, {3, 3}}; }
inline UnorderedMap initUnorderedMap() { return {{1, 3}, {3, 3}, {2, 2}}; }

#endif // TEST_INTEROP_CXX_STDLIB_INPUTS_STD_MAP_H
