// VersionVector.h — explicit per-device version tracking.
//
// Timestamps are never used to decide sync ordering (device clocks differ).
// Each file carries a map device_id -> counter; a client's edit bumps its own
// entry. Divergence (conflict) is detected by comparing vectors.
#pragma once

#include <map>
#include <string>

namespace npsync
{

class VersionVector {
  public:
    std::map<std::string, int64_t> entries;

    int64_t get(const std::string& device) const;
    void bump(const std::string& device);
    void merge(const VersionVector& other); // element-wise max

    // Ordering relations between two vectors.
    bool dominates(const VersionVector& o) const;  // >= everywhere and > somewhere
    bool concurrent(const VersionVector& o) const; // neither dominates -> conflict
    bool equal(const VersionVector& o) const {
        return entries == o.entries;
    }

    std::string toJson() const;
    static VersionVector fromJson(const std::string& json);
};

} // namespace npsync
