#include "VersionVector.h"

#include <cctype>
#include <sstream>

namespace npsync
{

int64_t VersionVector::get(const std::string& device) const {
    auto it = entries.find(device);
    return it == entries.end() ? 0 : it->second;
}

void VersionVector::bump(const std::string& device) {
    ++entries[device];
}

void VersionVector::merge(const VersionVector& other) {
    for (const auto& [dev, v] : other.entries)
        if (v > entries[dev])
            entries[dev] = v;
}

bool VersionVector::dominates(const VersionVector& o) const {
    bool greater = false;
    for (const auto& [dev, v] : entries) {
        if (v < o.get(dev))
            return false;
        if (v > o.get(dev))
            greater = true;
    }
    for (const auto& [dev, v] : o.entries)
        if (v > 0 && entries.count(dev) == 0)
            return false;
    return greater;
}

bool VersionVector::concurrent(const VersionVector& o) const {
    return !dominates(o) && !o.dominates(*this) && !equal(o);
}

std::string VersionVector::toJson() const {
    std::ostringstream ss;
    ss << "{";
    bool first = true;
    for (const auto& [dev, v] : entries) {
        if (!first)
            ss << ",";
        first = false;
        ss << "\"" << dev << "\":" << v;
    }
    ss << "}";
    return ss.str();
}

VersionVector VersionVector::fromJson(const std::string& json) {
    // Tolerant parser for {"dev":n,...} objects produced by toJson / server.
    VersionVector vv;
    size_t i = 0;
    auto skipWs = [&] {
        while (i < json.size() && isspace(static_cast<unsigned char>(json[i])))
            ++i;
    };
    skipWs();
    if (i >= json.size() || json[i] != '{')
        return vv;
    ++i;
    while (i < json.size()) {
        skipWs();
        if (i < json.size() && json[i] == '}')
            break;
        if (i >= json.size() || json[i] != '"')
            break;
        ++i;
        std::string key;
        while (i < json.size() && json[i] != '"')
            key.push_back(json[i++]);
        if (i < json.size())
            ++i; // closing quote
        skipWs();
        if (i < json.size() && json[i] == ':')
            ++i;
        skipWs();
        int64_t val = 0;
        bool neg = false;
        if (i < json.size() && json[i] == '-') {
            neg = true;
            ++i;
        }
        bool any = false;
        while (i < json.size() && isdigit(static_cast<unsigned char>(json[i]))) {
            val = val * 10 + (json[i] - '0');
            any = true;
            ++i;
        }
        if (any && !neg)
            vv.entries[key] = val;
        skipWs();
        if (i < json.size() && json[i] == ',')
            ++i;
    }
    return vv;
}

} // namespace npsync
