// Merge.h — three-way text merge for conflict resolution.
//
// Given the common base version and two divergent edits (local & remote),
// produces a merged text when the edits touch disjoint regions. Returns
// conflict hunks otherwise — the caller then asks the user.
// Line-oriented, whitespace-exact; never discards content silently.
#pragma once

#include <string>
#include <vector>

namespace npsync
{

struct MergeHunk
{
    // A conflicting region: lines where local and remote disagree.
    std::vector<std::string> localLines;
    std::vector<std::string> remoteLines;
};

struct MergeResult
{
    bool clean = false;           // true: merged without conflict
    std::string merged;           // valid only when clean == true
    std::vector<MergeHunk> hunks; // populated when clean == false
};

class ThreeWayMerge {
  public:
    static MergeResult merge(const std::string& base, const std::string& local, const std::string& remote);
};

// Split/join helpers (handle \n and \r\n; join uses the dominant EOL of base).
std::vector<std::string> splitLines(const std::string& text, std::string& eolOut);

} // namespace npsync
