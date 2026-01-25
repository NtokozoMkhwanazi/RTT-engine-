#include "Physics.h"
#include <glm/gtx/norm.hpp>
#include <glm/gtx/string_cast.hpp>
#include <algorithm>
#include <limits>
#include <cmath>
#include <sstream>
#include <iostream>

// -------------------- Helpers --------------------
static inline std::string vecToStr(const glm::vec3 &v, int prec = 4) {
    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(prec);
    ss << "(" << v.x << ", " << v.y << ", " << v.z << ")";
    return ss.str();
}
bool PhysicsWorld::raycastDown(
    const glm::vec3& origin,
    float maxDist,
    glm::vec3& hitPoint,
    glm::vec3& hitNormal)
{
    bool hit = false;
    float closestY = -std::numeric_limits<float>::max();

    for (auto& b : bodies)
    {
        if (!b || !b->isStatic) continue;

        glm::vec3 min = b->position - b->scale * 0.5f;
        glm::vec3 max = b->position + b->scale * 0.5f;

        if (origin.x < min.x || origin.x > max.x ||
            origin.z < min.z || origin.z > max.z)
            continue;

        float y = max.y;
        if (origin.y >= y && origin.y - maxDist <= y)
        {
            if (y > closestY)
            {
                closestY = y;
                hitPoint  = glm::vec3(origin.x, y, origin.z);
                hitNormal = glm::vec3(0, 1, 0);
                hit = true;
            }
        }
    }
    return hit;
}
// -------------------- OBB SAT --------------------
bool PhysicsWorld::obbOverlapAndPenetration(const OBB& A, const OBB& B, float& outPen, glm::vec3& outNormal) const {
    const float EPSILON = 1e-6f;
    float minPen = std::numeric_limits<float>::max();
    glm::vec3 bestAxis(0.0f);

    glm::vec3 axes[15];
    for (int i = 0; i < 3; ++i) axes[i] = A.axis[i];
    for (int i = 0; i < 3; ++i) axes[i+3] = B.axis[i];
    int idx = 6;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            axes[idx++] = glm::cross(A.axis[i], B.axis[j]);

    for (int i = 0; i < 15; ++i) {
        glm::vec3 axis = axes[i];
        if (glm::length2(axis) < EPSILON) continue;
        axis = glm::normalize(axis);

        float rA = A.half.x * std::abs(glm::dot(A.axis[0], axis)) +
                   A.half.y * std::abs(glm::dot(A.axis[1], axis)) +
                   A.half.z * std::abs(glm::dot(A.axis[2], axis));
        float rB = B.half.x * std::abs(glm::dot(B.axis[0], axis)) +
                   B.half.y * std::abs(glm::dot(B.axis[1], axis)) +
                   B.half.z * std::abs(glm::dot(B.axis[2], axis));
        float dist = std::abs(glm::dot(B.c - A.c, axis));
        float pen = rA + rB - dist;
        if (pen < 0.0f) return false;
        if (pen < minPen) {
            minPen = pen;
            bestAxis = axis * ((glm::dot(B.c - A.c, axis) < 0.0f) ? -1.0f : 1.0f);
        }
    }

    outPen = minPen;
    outNormal = bestAxis;
    return true;
}

// -------------------- Swept OBB vs OBB --------------------
bool PhysicsWorld::sweptOBBvsOBB(const OBB& a0, const glm::vec3& moveA,
                                 const OBB& b0, const glm::vec3& moveB,
                                 float& outTOI, glm::vec3& outNormal, float& outPenetration,
                                 int maxIter, float eps) const
{
    float t0 = 0.0f, t1 = 1.0f;
    glm::vec3 bestNormal;
    float bestPen = 0.0f;

    for (int iter = 0; iter < maxIter; ++iter) {
        float t = (t0 + t1) * 0.5f;
        OBB a = a0; a.c += moveA * t;
        OBB b = b0; b.c += moveB * t;

        float pen; glm::vec3 normal;
        if (obbOverlapAndPenetration(a, b, pen, normal)) {
            t1 = t;
            bestPen = pen;
            bestNormal = normal;
        } else {
            t0 = t;
        }
        if (t1 - t0 < eps) break;
    }

    if (bestPen > 0.0f) {
        outTOI = t1;
        outNormal = bestNormal;
        outPenetration = bestPen;
        return true;
    }
    return false;
}

// -------------------- Build OBB --------------------
OBB PhysicsWorld::buildOBBFromBody(const std::shared_ptr<RigidBody>& rb) const {
    OBB obb;
    obb.c = rb->position;
    obb.half = rb->scale * 0.5f;
    glm::mat3 rot = glm::mat3_cast(rb->rotation);
    obb.axis[0] = glm::normalize(rot[0]);
    obb.axis[1] = glm::normalize(rot[1]);
    obb.axis[2] = glm::normalize(rot[2]);
    return obb;
}

// -------------------- Ground check --------------------
bool PhysicsWorld::isGrounded(std::shared_ptr<RigidBody>& body, float probeDistance) {
    if (!body) return false;
    glm::vec3 bottom = body->position - glm::vec3(0.0f, body->scale.y * 0.5f, 0.0f);
    glm::vec3 probeEnd = bottom - glm::vec3(0.0f, probeDistance, 0.0f);
    
   const float EPSILON = 1e-6f;
    
    for (auto& other : bodies) {
        if (!other || other == body || !other->isStatic) continue;
        glm::vec3 otherMin = other->position - other->scale * 0.5f;
        glm::vec3 otherMax = other->position + other->scale * 0.5f;
        if (bottom.x < otherMax.x && bottom.x > otherMin.x &&
            bottom.z < otherMax.z && bottom.z > otherMin.z &&
            probeEnd.y <= otherMax.y && bottom.y >= otherMax.y)
            return true;
    
       glm::vec3 bMin = body->position - body->scale * 0.5f;
       glm::vec3 bMax = body->position + body->scale * 0.5f;
       glm::vec3 oMin = other->position - other->scale * 0.5f;
       glm::vec3 oMax = other->position + other->scale * 0.5f;

       bool xOverlap = (bMax.x > oMin.x) && (bMin.x < oMax.x);
       bool zOverlap = (bMax.z > oMin.z) && (bMin.z < oMax.z);
       bool yClose   = (bMin.y - oMax.y) < probeDistance && (bMin.y - oMax.y) > -probeDistance;

       if (xOverlap && zOverlap && yClose) return true;

    }
    return false;
}

// -------------------- Add/Clear --------------------
void PhysicsWorld::addBody(const std::shared_ptr<RigidBody>& body) { if (body) bodies.push_back(body); }
void PhysicsWorld::clear() { bodies.clear(); }

// -------------------- Adaptive substep --------------------
int PhysicsWorld::computeAdaptiveSubsteps(float dt) const {
    float maxVel = 0.0f;
    float minSize = std::numeric_limits<float>::max();
    for (auto& b : bodies) {
        if (!b || b->isStatic) continue;
        float vel = glm::length(b->velocity) * (1.0f + b->restitution);
        maxVel = std::max(maxVel, vel);
        minSize = std::min({minSize, b->scale.x, b->scale.y, b->scale.z});
    }
    if (maxVel < 1e-6f) return 1;
    int substeps = std::ceil(maxVel * dt / (0.2f * minSize));
    return std::clamp(substeps, 1, 12);
}

// -------------------- Sweep-and-prune --------------------
void PhysicsWorld::getPotentialPairs(std::vector<std::pair<int,int>>& outPairs) {
    outPairs.clear();
    if (bodies.empty()) return;

    struct Interval { float minx, maxx; int idx; };
    std::vector<Interval> intervals;
    intervals.reserve(bodies.size());

    for (size_t i = 0; i < bodies.size(); ++i) {
        auto& b = bodies[i];
        if (!b) continue;
        float minx = b->position.x - b->scale.x * 0.5f;
        float maxx = b->position.x + b->scale.x * 0.5f;
        intervals.push_back({minx, maxx, int(i)});
    }

    std::sort(intervals.begin(), intervals.end(), [](const Interval& a, const Interval& b){ return a.minx < b.minx; });

    for (size_t i = 0; i < intervals.size(); ++i) {
        for (size_t j = i + 1; j < intervals.size(); ++j) {
            if (intervals[j].minx > intervals[i].maxx) break;
            outPairs.emplace_back(intervals[i].idx, intervals[j].idx);
        }
    }
}

// -------------------- Resolve contact --------------------
static void resolveContact(std::shared_ptr<RigidBody>& a,
                           std::shared_ptr<RigidBody>& b,
                           const glm::vec3& normal,
                           float penetration)
{
    if (!a || !b) return;

    bool aImmovable = a->isStatic || a->isModel;
    bool bImmovable = b->isStatic || b->isModel;

    float invMassA = aImmovable ? 0.0f : 1.0f / a->mass;
    float invMassB = bImmovable ? 0.0f : 1.0f / b->mass;
    float totalInvMass = invMassA + invMassB;

    if (totalInvMass < 1e-6f) return; // both immovable, skip

    // --- Position correction (penetration resolution) ---
    glm::vec3 correction = normal * penetration / totalInvMass * 0.8f; // 80% factor for stability
    if (!aImmovable) a->position -= correction * invMassA;
    if (!bImmovable) b->position += correction * invMassB;

    // --- Relative velocity along normal ---
    glm::vec3 relVel = b->velocity - a->velocity;
    float velAlongNormal = glm::dot(relVel, normal);

    // Bodies separating? Skip impulse
    if (velAlongNormal > 0.0f) return;

    // --- Restitution ---
    float e = std::min(a->restitution, b->restitution);
    // If one body immovable and normal is mostly vertical, zero bounce
    if ((aImmovable || bImmovable) && std::abs(normal.y) > 0.5f)
        e = 0.0f;

    // --- Impulse ---
    float j = -(1.0f + e) * velAlongNormal / totalInvMass;
    glm::vec3 impulse = j * normal;

    if (!aImmovable) a->velocity -= impulse * invMassA;
    if (!bImmovable) b->velocity += impulse * invMassB;

    // --- Friction ---
    glm::vec3 tangent = relVel - glm::dot(relVel, normal) * normal;
    if (glm::length2(tangent) > 1e-6f) {
        tangent = glm::normalize(tangent);
        float jt = -glm::dot(relVel, tangent) / totalInvMass;
        float frictionCoeff = std::sqrt(a->friction * b->friction);
        float maxJt = j * frictionCoeff;
        jt = std::clamp(jt, -maxJt, maxJt);
        glm::vec3 frictionImpulse = jt * tangent;

        if (!aImmovable) a->velocity -= frictionImpulse * invMassA;
        if (!bImmovable) b->velocity += frictionImpulse * invMassB;
    }

    // --- Clamp tiny velocities ---
    if (!aImmovable && glm::length2(a->velocity) < 1e-6f) a->velocity = glm::vec3(0.0f);
    if (!bImmovable && glm::length2(b->velocity) < 1e-6f) b->velocity = glm::vec3(0.0f);

    // --- Zero rotation ---
    if (!aImmovable) a->angularVelocity = glm::vec3(0.0f);
    if (!bImmovable) b->angularVelocity = glm::vec3(0.0f);
}


// -------------------- Physics step (rotation disabled, models optionally immovable) --------------------
void PhysicsWorld::step(float dt)
{
    if (bodies.empty()) return;

    int substeps = computeAdaptiveSubsteps(dt);
    float subdt = dt / float(substeps);

    for (int step = 0; step < substeps; ++step) {

        // --- integrate linear & angular motion ---
        for (auto& b : bodies) {
            if (!b || b->isStatic) continue;

            // Save previous transforms
            b->prevPosition = b->position;
            b->prevRotation = b->rotation;

            // -------- LINEAR --------
            if (!b->isModel) {
                if (glm::length2(b->forceAccumulator) > 0.0f) {
                    b->velocity += b->forceAccumulator * b->invMass() * subdt;
                }
                b->velocity += gravity * subdt;
            }

            b->position += b->velocity * subdt;

            // -------- ANGULAR (QUATERNION) --------
            if (!b->isModel) {
                // Angular acceleration (world space)
                glm::vec3 angAccel = b->inertiaLocalInv * b->torqueAccumulator;

                b->angularVelocity += angAccel * subdt;

                float angSpeed = glm::length(b->angularVelocity);
                if (angSpeed > 1e-5f) {
                    glm::vec3 axis = b->angularVelocity / angSpeed;
                    glm::quat dq = glm::angleAxis(angSpeed * subdt, axis);
                    b->rotation = glm::normalize(dq * b->rotation);
                }
            }

            // Clear accumulators
            b->forceAccumulator = glm::vec3(0.0f);
            b->torqueAccumulator = glm::vec3(0.0f);
        }

        // --- collision detection ---
        std::vector<std::pair<int,int>> pairs;
        getPotentialPairs(pairs);

        for (auto& pr : pairs) {
            int i = pr.first;
            int j = pr.second;
            if (i < 0 || j < 0 || i >= (int)bodies.size() || j >= (int)bodies.size()) continue;
            auto& A = bodies[i];
            auto& B = bodies[j];
            if (!A || !B) continue;

            OBB obbA = buildOBBFromBody(A);
            OBB obbB = buildOBBFromBody(B);

            glm::vec3 moveA = A->position - A->prevPosition;
            glm::vec3 moveB = B->position - B->prevPosition;

            float toi = 0.0f, pen = 0.0f;
            glm::vec3 normal(0.0f);

            if (sweptOBBvsOBB(obbA, moveA, obbB, moveB, toi, normal, pen)) {
                resolveContact(A, B, normal, pen);
            }
        }

        // --- ground snap & friction ---
        for (auto& b : bodies) {
            if (!b || b->isStatic) continue;

            b->onGround = false;

            if (glm::length2(b->velocity) < 1e-6f)
                b->velocity = glm::vec3(0.0f);

            if (isGrounded(b, 1e-3f)) {
                b->velocity.y = 0.0f;
                b->onGround = true;
            }

            if (b->onGround && !b->isModel) {
                float friction = 5.0f;
                b->velocity.x -= b->velocity.x * friction * subdt;
                b->velocity.z -= b->velocity.z * friction * subdt;

                if (glm::length2(glm::vec2(b->velocity.x, b->velocity.z)) < 1e-6f)
                    b->velocity.x = b->velocity.z = 0.0f;
            }
        }
    }
}

