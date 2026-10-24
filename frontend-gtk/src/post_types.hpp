// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <map>
#include <string>
#include <vector>

namespace ds::frontend {

// One user-adjustable parameter of a post-processing shader.
struct PostOption {
    enum class Type { Bool, Int, Float };
    std::string name;       // uniform name
    std::string label;
    Type  type  = Type::Float;
    float min   = 0.f;
    float max   = 1.f;
    float step  = 0.1f;
    float def   = 0.f;
    std::vector<std::string> items;   // names of the values min..max, for a list
};

// One stage of the post-processing chain as stored in the config.
struct PostStage {
    std::string shader;                       // library name, e.g. "dreamstation/crt"
    bool        enabled = true;
    std::map<std::string, float> options;     // only values that differ from default
};

} // namespace ds::frontend
