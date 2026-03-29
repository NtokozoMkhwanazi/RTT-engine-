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
            json["visible"] = m.visible;
            json["castShadows"] = m.castShadows;
            json["receiveShadows"] = m.receiveShadows;
            json["materialID"] = m.materialID;
            json["layer"] = m.layer;
            return json;
        },
        [](const Json::Value& json) -> MeshComponent {
            MeshComponent m;
            m.meshID = json["meshID"].asInt();
            m.visible = json["visible"].asBool();
            m.castShadows = json["castShadows"].asBool();
            m.receiveShadows = json["receiveShadows"].asBool();
            m.materialID = json["materialID"].asInt();
            m.layer = json["layer"].asUInt();
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
            json["orthographic"] = c.orthographic;
            json["orthographicSize"] = c.orthographicSize;
            return json;
        },
        [](const Json::Value& json) -> CameraComponent {
            CameraComponent c;
            c.fov = json["fov"].asFloat();
            c.nearPlane = json["nearPlane"].asFloat();
            c.farPlane = json["farPlane"].asFloat();
            c.aspectRatio = json["aspectRatio"].asFloat();
            c.isActive = json["isActive"].asBool();
            c.orthographic = json["orthographic"].asBool();
            c.orthographicSize = json["orthographicSize"].asFloat();
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
            json["spotAngle"] = l.spotAngle;
            json["spotOuterAngle"] = l.spotOuterAngle;
            json["shadowsEnabled"] = l.shadowsEnabled;
            return json;
        },
        [](const Json::Value& json) -> LightComponent {
            LightComponent l;
            l.type = static_cast<LightType>(json["type"].asInt());
            l.enabled = json["enabled"].asBool();
            l.color = JsonHelpers::fromJsonVec3(json["color"]);
            l.intensity = json["intensity"].asFloat();
            l.range = json["range"].asFloat();
            l.spotAngle = json["spotAngle"].asFloat();
            l.spotOuterAngle = json["spotOuterAngle"].asFloat();
            l.shadowsEnabled = json["shadowsEnabled"].asBool();
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
            t.tag = json["tag"].asString();
            return t;
        }
    );
}

} // namespace ecs
