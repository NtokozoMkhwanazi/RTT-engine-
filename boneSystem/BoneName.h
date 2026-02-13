#pragma once
#include <string>
#include <algorithm>
#include <cctype>

// ========================================
// CANONICAL BONE NAME NORMALIZATION
// ========================================
inline std::string NormalizeBoneName(const std::string& name)
{
    std::string n = name;

    // Convert to lowercase
    std::transform(n.begin(), n.end(), n.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Remove hierarchy (pipe |)
    size_t p = n.find('|');
    if (p != std::string::npos)
        n = n.substr(p + 1);

    // Remove namespace (mixamorig: or armature:)
    p = n.find(':');
    if (p != std::string::npos)
        n = n.substr(p + 1);

    // Remove underscores and spaces
    n.erase(std::remove_if(n.begin(), n.end(),
                           [](char c) { return c == '_' || c == ' '; }),
            n.end());

    return n;
}

