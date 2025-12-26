#pragma once
#include <memory>

// Base class for components
struct Component {
    virtual ~Component() = default;
};

using ComponentPtr = std::unique_ptr<Component>;
