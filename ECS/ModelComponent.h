// ModelComponent.h
#pragma once
#include <memory>
#include "Model.h"
#include "Component.h"

struct ModelComponent : public Component {
    std::shared_ptr<Model> model;
    ModelComponent() = default;
    ModelComponent(std::shared_ptr<Model> m) : model(m) {}
};
