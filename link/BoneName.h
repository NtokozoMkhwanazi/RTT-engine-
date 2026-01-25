#pragma once
#include <string>

inline std::string NormalizeBoneName(const std::string& name)
{
    std::string n = name;

    // Remove hierarchy
    size_t p = n.find('|');
    if (p != std::string::npos)
        n = n.substr(p + 1);

    // Remove namespace (mixamorig:)
    p = n.find(':');
    if (p != std::string::npos)
        n = n.substr(p + 1);

    return n;
}

