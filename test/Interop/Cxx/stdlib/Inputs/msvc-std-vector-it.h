#pragma once

#include <vector>
#include <string>

struct FetchProvidersResult {
  std::vector<std::string> providers;
};

inline FetchProvidersResult *f() noexcept {
    return new FetchProvidersResult();
}
