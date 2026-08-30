#include "IgnoreRules.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace npsync {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

} // namespace

IgnoreRules IgnoreRules::parse(const std::string& text) {
    IgnoreRules rules;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        Rule r;
        if (line[0] == '!') {
            r.negate = true;
            line = trim(line.substr(1));
            if (line.empty()) continue;
        }
        if (line.back() == '/') {
            r.dirOnly = true;
            line.pop_back();
        }
        if (!line.empty() && line[0] == '/') {
            r.anchored = true;
            line.erase(line.begin());
        }
        if (line.empty()) continue;
        r.pattern = lower(line);
        rules.rules_.push_back(std::move(r));
    }
    return rules;
}

bool IgnoreRules::ignored(const std::string& relPath, bool isDir) const {
    std::string p = lower(relPath);
    std::replace(p.begin(), p.end(), '\\', '/');
    bool result = false;
    for (const auto& r : rules_) {
        if (matchRule(r, p, isDir)) result = !r.negate;
    }
    return result;
}

bool IgnoreRules::matchRule(const Rule& r, const std::string& relPath, bool isDir) {
    if (r.dirOnly && !isDir) {
        // A dir-only rule still ignores everything *under* a matched dir,
        // which is handled by testing parent segments below.
    }
    // Test the full path and every parent directory prefix so that
    // "node_modules/" ignores node_modules/src/index.js too.
    std::vector<std::string> candidates;
    candidates.push_back(relPath);
    for (size_t pos = relPath.find('/'); pos != std::string::npos; pos = relPath.find('/', pos + 1))
        candidates.push_back(relPath.substr(0, pos));

    for (const auto& cand : candidates) {
        bool candIsDir = (cand != relPath) || isDir;
        if (r.dirOnly && !candIsDir) continue;

        if (r.anchored || r.pattern.find('/') != std::string::npos) {
            // Path-relative match against the candidate.
            if (globMatch(r.pattern, cand)) return true;
        } else {
            // Basename match against the candidate's last segment.
            size_t slash = cand.find_last_of('/');
            std::string base = (slash == std::string::npos) ? cand : cand.substr(slash + 1);
            if (globMatch(r.pattern, base)) return true;
        }
    }
    return false;
}

// Recursive glob matcher supporting * ? and ** (crosses directories).
bool IgnoreRules::globMatch(const std::string& pat, const std::string& str) {
    size_t p = 0, s = 0;
    size_t starP = std::string::npos, starS = 0;
    bool doubleStar = false;

    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) {
            ++p; ++s;
        } else if (p + 1 < pat.size() && pat[p] == '*' && pat[p + 1] == '*') {
            // ** matches any number of segments (including none).
            starP = p; starS = s; doubleStar = true;
            p += 2;
            if (p < pat.size() && pat[p] == '/') ++p; // " ** / " collapses
        } else if (p < pat.size() && pat[p] == '*') {
            starP = p; starS = s; doubleStar = false;
            ++p;
        } else if (starP != std::string::npos) {
            // Backtrack to the last star.
            if (!doubleStar && str[starS] == '/') return false; // single * can't cross /
            ++starS;
            p = (doubleStar && starP + 2 < pat.size() && pat[starP + 2] == '/')
                    ? starP + 3 : starP + (doubleStar ? 2 : 1);
            s = starS;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

} // namespace npsync
