#pragma once

/**
 * Component Serializers - Registration for Transform, Mesh, RigidBody components
 *
 * Include this file to register all default component serializers
 * for the ECS serialization system.
 */

#include "Serialization.h"
#include "components/Components.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstring>

namespace ecs {

/**
 * Helper functions for GLM serialization
 */
namespace JsonHelpers {

inline Json::Value toJson(const glm::vec3& v) {
    Json::Value json;
    json.append(v.x);
    json.append(v.y);
    json.append(v.z);
    return json;
}

inline glm::vec3 fromJsonVec3(const Json::Value& json) {
    return glm::vec3(
        json[0].asFloat(),
        json[1].asFloat(),
        json[2].asFloat()
    );
}

inline Json::Value toJson(const glm::quat& q) {
    Json::Value json;
    json.append(q.x);
    json.append(q.y);
    json.append(q.z);
    json.append(q.w);
    return json;
}

inline glm::quat fromJsonQuat(const Json::Value& json) {
    return glm::quat(
        json[3].asFloat(),  // w
        json[0].asFloat(),  // x
        json[1].asFloat(),  // y
        json[2].asFloat()   // z
    );
}

inline Json::Value toJson(const glm::mat4& m) {
    Json::Value json;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            json.append(m[i][j]);
        }
    }
    return json;
}

inline glm::mat4 fromJsonMat4(const Json::Value& json) {
    glm::mat4 m;
    int index = 0;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            m[i][j] = json[index++].asFloat();
        }
    }
    return m;
}

} // namespace JsonHelpers

/**
 * Register all default component serializers
 */
inline void registerDefaultComponentSerializers() {
    auto& registry = SerializerRegistry::getInstance();

    // TransformComponent serializer
    registry.registerSerializer<TransformComponent>(
        [](const TransformComponent& t) -> Json::Value {
            Json::Value json;
            json["position"] = JsonHelpers::toJson(t.position);
            json["rotation"] = JsonHelpers::toJson(t.rotation);
            json["scale"] = JsonHelpers::toJson(t.scale);
            return json;
        },
        [](const Json::Value& json) -> TransformComponent {
            TransformComponent t;
            t.position = JsonHelpers::fromJsonVec3(json["position"]);
            t.rotation = JsonHelpers::fromJsonQuat(json["rotation"]);
            t.scale = JsonHelpers::fromJsonVec3(json["scale"]);
            return t;
        }
    );

    // MeshComponent serializer
    registry.registerSerializer<MeshComponent>(
        [](const MeshComponent& m) -> Json::Value {
            Json::Value json;
            json["meshID"] = m.meshID;
            json["meshType"] = static_cast<int>(m.meshType);
            json["visible"] = m.visible;
            json["castShadows"] = m.castShadow;
            json["receiveShadows"] = m.receiveShadow;
            json["color"] = JsonHelpers::toJson(m.color);
            json["metallic"] = m.metallic;
            json["roughness"] = m.roughness;
            json["alpha"] = m.alpha;
            return json;
        },
        [](const Json::Value& json) -> MeshComponent {
            MeshComponent m;
            m.meshID = json["meshID"].asInt();
            m.meshType = static_cast<MeshType>(json["meshType"].asInt());
            m.visible = json["visible"].asBool();
            m.castShadow = json["castShadows"].asBool();
            m.receiveShadow = json["receiveShadows"].asBool();
            m.color = JsonHelpers::fromJsonVec3(json["color"]);
            m.metallic = json["metallic"].asFloat();
            m.roughness = json["roughness"].asFloat();
            m.alpha = json["alpha"].asFloat();
            return m;
        }
    );

    // RigidBodyComponent serializer
    registry.registerSerializer<RigidBodyComponent>(
        [](const RigidBodyComponent& r) -> Json::Value {
            Json::Value json;
            json["bodyType"] = static_cast<int>(r.bodyType);
            json["colliderType"] = static_cast<int>(r.colliderType);
            json["mass"] = r.invMass > 0 ? 1.0f / r.invMass : 0.0f;
            json["restitution"] = r.restitution;
            json["friction"] = r.friction;
            json["linearDamping"] = r.linearDamping;
            json["angularDamping"] = r.angularDamping;
            json["useGravity"] = r.useGravity;
            json["isKinematic"] = r.bodyType == RigidBodyType::KINEMATIC;
            
            // Collider specifics
            if (r.colliderType == ColliderType::SPHERE) {
                json["sphereRadius"] = r.sphereRadius;
            } else if (r.colliderType == ColliderType::BOX) {
                json["boxSize"] = JsonHelpers::toJson(r.boxSize);
            }
            
            // Current state (optional, can be excluded for save games)
            json["linearVelocity"] = JsonHelpers::toJson(r.linearVelocity);
            json["angularVelocity"] = JsonHelpers::toJson(r.angularVelocity);
            
            return json;
        },
        [](const Json::Value& json) -> RigidBodyComponent {
            RigidBodyComponent r;
            
            r.bodyType = static_cast<RigidBodyType>(json["bodyType"].asInt());
            r.colliderType = static_cast<ColliderType>(json["colliderType"].asInt());
            
            // Set mass from JSON (convert to inverse mass)
            float mass = json["mass"].asFloat();
            r.invMass = mass > 0 ? 1.0f / mass : 0.0f;
            
            r.restitution = json["restitution"].asFloat();
            r.friction = json["friction"].asFloat();
            r.linearDamping = json["linearDamping"].asFloat();
            r.angularDamping = json["angularDamping"].asFloat();
            r.useGravity = json["useGravity"].asBool();
            
            // Override body type if kinematic flag is set
            if (json.isMember("isKinematic") && json["isKinematic"].asBool()) {
                r.bodyType = RigidBodyType::KINEMATIC;
                r.invMass = 0.0f;
            }
            
            // Collider specifics
            if (r.colliderType == ColliderType::SPHERE) {
                r.sphereRadius = json["sphereRadius"].asFloat();
            } else if (r.colliderType == ColliderType::BOX) {
                r.boxSize = JsonHelpers::fromJsonVec3(json["boxSize"]);
            }
            
            // Current state
            r.linearVelocity = JsonHelpers::fromJsonVec3(json["linearVelocity"]);
            r.angularVelocity = JsonHelpers::fromJsonVec3(json["angularVelocity"]);
            
            return r;
        }
    );

    // CameraComponent serializer
    registry.registerSerializer<CameraComponent>(
        [](const CameraComponent& c) -> Json::Value {
            Json::Value json;
            json["fov"] = c.fov;
            json["nearPlane"] = c.nearPlane;
            json["farPlane"] = c.farPlane;
            json["aspectRatio"] = c.aspectRatio;
            json["isActive"] = c.isActive;
            json["orthographic"] = c.isOrthographic;
            json["orthographicSize"] = c.orthoSize;
            return json;
        },
        [](const Json::Value& json) -> CameraComponent {
            CameraComponent c;
            c.fov = json["fov"].asFloat();
            c.nearPlane = json["nearPlane"].asFloat();
            c.farPlane = json["farPlane"].asFloat();
            c.aspectRatio = json["aspectRatio"].asFloat();
            c.isActive = json["isActive"].asBool();
            c.isOrthographic = json["orthographic"].asBool();
            c.orthoSize = json["orthographicSize"].asFloat();
            return c;
        }
    );

    // LightComponent serializer
    registry.registerSerializer<LightComponent>(
        [](const LightComponent& l) -> Json::Value {
            Json::Value json;
            json["type"] = static_cast<int>(l.type);
            json["enabled"] = l.enabled;
            json["color"] = JsonHelpers::toJson(l.color);
            json["intensity"] = l.intensity;
            json["range"] = l.range;
            json["spotAngle"] = l.spotInnerAngle;
            json["spotOuterAngle"] = l.spotOuterAngle;
            json["shadowsEnabled"] = l.castShadows;
            return json;
        },
        [](const Json::Value& json) -> LightComponent {
            LightComponent l;
            l.type = static_cast<LightType>(json["type"].asInt());
            l.enabled = json["enabled"].asBool();
            l.color = JsonHelpers::fromJsonVec3(json["color"]);
            l.intensity = json["intensity"].asFloat();
            l.range = json["range"].asFloat();
            l.spotInnerAngle = json["spotAngle"].asFloat();
            l.spotOuterAngle = json["spotOuterAngle"].asFloat();
            l.castShadows = json["shadowsEnabled"].asBool();
            return l;
        }
    );

    // AnimatorComponent serializer
    registry.registerSerializer<AnimatorComponent>(
        [](const AnimatorComponent& a) -> Json::Value {
            Json::Value json;
            json["isPlaying"] = a.isPlaying;
            json["currentAnimation"] = a.currentAnimation;
            json["playbackSpeed"] = a.playbackSpeed;
            json["loop"] = a.loop;
            json["useRootMotion"] = a.useRootMotion;
            json["blendDuration"] = a.blendDuration;
            return json;
        },
        [](const Json::Value& json) -> AnimatorComponent {
            AnimatorComponent a;
            a.isPlaying = json["isPlaying"].asBool();
            a.currentAnimation = json["currentAnimation"].asInt();
            a.playbackSpeed = json["playbackSpeed"].asFloat();
            a.loop = json["loop"].asBool();
            a.useRootMotion = json["useRootMotion"].asBool();
            a.blendDuration = json["blendDuration"].asFloat();
            return a;
        }
    );

    // TagComponent serializer
    registry.registerSerializer<TagComponent>(
        [](const TagComponent& t) -> Json::Value {
            Json::Value json;
            json["tag"] = t.tag;
            return json;
        },
        [](const Json::Value& json) -> TagComponent {
            TagComponent t;
            const std::string tag = json["tag"].asString();
            std::strncpy(t.tag, tag.c_str(), MAX_TAG_LENGTH - 1);
            t.tag[MAX_TAG_LENGTH - 1] = '\0';
            return t;
        }
    );

    // ParentComponent serializer (entity-ref component -> participates in the
    // Entity-ID translation engine via the 3rd remap callback).
    registry.registerSerializer<ParentComponent>(
        [](const ParentComponent& p) -> Json::Value {
            Json::Value json;
            json["parent"] = static_cast<Json::UInt64>(p.parent.id);
            return json;
        },
        [](const Json::Value& json) -> ParentComponent {
            ParentComponent p;
            p.parent = Entity{ static_cast<EntityID>(json["parent"].asUInt64()) };
            return p;
        },
        [](void* component, const std::unordered_map<EntityID, EntityID>& idMap) {
            auto* p = static_cast<ParentComponent*>(component);
            EntityID oldParent = p->parent.id;
            if (oldParent == INVALID_ENTITY_ID) return;
            auto it = idMap.find(oldParent);
            p->parent = (it != idMap.end()) ? Entity{it->second} : Entity{INVALID_ENTITY_ID};
        }
    );

    // ChildrenComponent serializer (entity-ref component -> ID translation engine).
    registry.registerSerializer<ChildrenComponent>(
        [](const ChildrenComponent& c) -> Json::Value {
            Json::Value json;
            json["count"] = static_cast<Json::UInt64>(c.count);
            Json::Value arr(Json::arrayValue);
            for (size_t i = 0; i < c.count; i++) {
                arr.append(static_cast<Json::UInt64>(c.children[i].id));
            }
            json["children"] = arr;
            return json;
        },
        [](const Json::Value& json) -> ChildrenComponent {
            ChildrenComponent c;
            c.count = static_cast<size_t>(json["count"].asUInt64());
            const Json::Value& arr = json["children"];
            for (Json::ArrayIndex i = 0; i < arr.size() && static_cast<size_t>(i) < 16; i++) {
                c.children[i] = Entity{ static_cast<EntityID>(arr[i].asUInt64()) };
            }
            return c;
        },
        [](void* component, const std::unordered_map<EntityID, EntityID>& idMap) {
            auto* c = static_cast<ChildrenComponent*>(component);
            size_t write = 0;
            for (size_t i = 0; i < c->count; i++) {
                EntityID oldChild = c->children[i].id;
                if (oldChild == INVALID_ENTITY_ID) continue;
                auto it = idMap.find(oldChild);
                if (it != idMap.end()) {
                    c->children[write++] = Entity{it->second};
                }
            }
            c->count = write;
        }
    );
}

} // namespace ecs
