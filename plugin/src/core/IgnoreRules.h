// IgnoreRules.h — .gitignore-style exclusion patterns for sync roots.
//
// Supported syntax (subset of gitignore):
//   *.tmp          match by filename glob in any directory
//   build/         directory-only match
//   /foo.txt       anchored to the sync root
//   **             any number of path segments
//   !keep.tmp      negation (re-include)
//   # comment
// Matching is case-insensitive (Windows semantics).
#pragma once

#include <string>
#include <vector>

namespace npsync
{

class IgnoreRules {
  public:
    // Parse rules from text (one pattern per line).
    static IgnoreRules parse(const std::string& text);

    // relPath uses forward slashes and is relative to the sync root.
    // isDir tells whether the path is a directory (affects "dir/" rules).
    bool ignored(const std::string& relPath, bool isDir) const;

    bool empty() const {
        return rules_.empty();
    }

  private:
    struct Rule
    {
        std::string pattern;
        bool negate = false;
        bool dirOnly = false;
        bool anchored = false;
    };
    std::vector<Rule> rules_;

    static bool matchRule(const Rule& r, const std::string& relPath, bool isDir);
    static bool globMatch(const std::string& pat, const std::string& str);
};

} // namespace npsync
