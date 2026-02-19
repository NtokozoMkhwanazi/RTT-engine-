#include "Physics.h"
#include <glm/gtx/norm.hpp>
#include <glm/gtx/string_cast.hpp>
#include <glm/gtx/intersect.hpp>
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

// -------------------- Raycasting --------------------
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

bool PhysicsWorld::raycast(
    const glm::vec3& origin,
    const glm::vec3& direction,
    float maxDist,
    glm::vec3& hitPoint,
    glm::vec3& hitNormal,
    std::shared_ptr<RigidBody>& hitBody)
{
    bool hit = false;
    float closestDist = std::numeric_limits<float>::max();

    for (auto& b : bodies)
    {
        if (!b) continue;

        // Simple AABB-ray intersection test
        glm::vec3 min = b->position - b->scale * 0.5f;
        glm::vec3 max = b->position + b->scale * 0.5f;

        // Find intersection with AABB
        float t1 = (min.x - origin.x) / direction.x;
        float t2 = (max.x - origin.x) / direction.x;
        float t3 = (min.y - origin.y) / direction.y;
        float t4 = (max.y - origin.y) / direction.y;
        float t5 = (min.z - origin.z) / direction.z;
        float t6 = (max.z - origin.z) / direction.z;

        float tmin = std::max(std::max(std::min(t1, t2), std::min(t3, t4)), std::min(t5, t6));
        float tmax = std::min(std::min(std::max(t1, t2), std::max(t3, t4)), std::max(t5, t6));

        // Ray intersects AABB
        if (tmax >= 0.0f && tmin <= tmax && tmin < closestDist && tmin >= 0.0f)
        {
            glm::vec3 intersection = origin + direction * tmin;
            float dist = glm::distance(origin, intersection);

            if (dist < closestDist && dist <= maxDist)
            {
                closestDist = dist;
                hitPoint = intersection;

                // Determine which face was hit to set normal
                if (tmin == t1) hitNormal = glm::vec3(-1, 0, 0);
                else if (tmin == t2) hitNormal = glm::vec3(1, 0, 0);
                else if (tmin == t3) hitNormal = glm::vec3(0, -1, 0);
                else if (tmin == t4) hitNormal = glm::vec3(0, 1, 0);
                else if (tmin == t5) hitNormal = glm::vec3(0, 0, -1);
                else hitNormal = glm::vec3(0, 0, 1);

                hitBody = b;
                hit = true;
            }
        }
    }

    return hit;
}

void PhysicsWorld::removeBody(const std::shared_ptr<RigidBody>& body)
{
    bodies.erase(
        std::remove_if(bodies.begin(), bodies.end(),
            [&body](const std::shared_ptr<RigidBody>& b) { return b == body; }),
        bodies.end());
}

std::vector<std::shared_ptr<RigidBody>> PhysicsWorld::getBodiesInAABB(const glm::vec3& min, const glm::vec3& max) const
{
    std::vector<std::shared_ptr<RigidBody>> result;

    for (const auto& body : bodies)
    {
        if (!body) continue;

        glm::vec3 bodyMin = body->position - body->scale * 0.5f;
        glm::vec3 bodyMax = body->position + body->scale * 0.5f;

        // Check if AABBs overlap
        if (bodyMin.x <= max.x && bodyMax.x >= min.x &&
            bodyMin.y <= max.y && bodyMax.y >= min.y &&
            bodyMin.z <= max.z && bodyMax.z >= min.z)
        {
            result.push_back(body);
        }
    }

    return result;
}

std::shared_ptr<RigidBody> PhysicsWorld::getBodyAtPoint(const glm::vec3& point, float radius) const
{
    for (const auto& body : bodies)
    {
        if (!body) continue;

        float dist = glm::distance(point, body->position);
        if (dist <= radius)
        {
            return body;
        }
    }

    return nullptr;
}

// -------------------- Shape building --------------------
Sphere PhysicsWorld::buildSphereFromBody(const std::shared_ptr<RigidBody>& rb) const
{
    Sphere sphere;
    sphere.center = rb->position;
    sphere.radius = (rb->scale.x + rb->scale.y + rb->scale.z) / 3.0f; // Average scale for radius
    return sphere;
}

Capsule PhysicsWorld::buildCapsuleFromBody(const std::shared_ptr<RigidBody>& rb) const
{
    Capsule capsule;
    capsule.center = rb->position;
    capsule.radius = rb->scale.x; // Assume x-scale is radius
    capsule.height = rb->scale.y; // Assume y-scale is height
    capsule.axis = glm::vec3(0.0f, 1.0f, 0.0f); // Default Y-axis orientation
    return capsule;
}

// -------------------- Collision Detection --------------------
CollisionResult PhysicsWorld::checkCollision(const std::shared_ptr<RigidBody>& a, const std::shared_ptr<RigidBody>& b) const
{
    CollisionResult result;

    // Determine collision based on collider types
    if (a->colliderType == ColliderType::SPHERE && b->colliderType == ColliderType::SPHERE) {
        Sphere sa = buildSphereFromBody(a);
        Sphere sb = buildSphereFromBody(b);
        result = checkSphereVsSphere(sa, sb);
    }
    else if (a->colliderType == ColliderType::BOX && b->colliderType == ColliderType::SPHERE) {
        OBB box = buildOBBFromBody(a);
        Sphere sph = buildSphereFromBody(b);
        result = checkBoxVsSphere(box, sph);
    }
    else if (a->colliderType == ColliderType::SPHERE && b->colliderType == ColliderType::BOX) {
        Sphere sph = buildSphereFromBody(a);
        OBB box = buildOBBFromBody(b);
        result = checkBoxVsSphere(box, sph); // Same as above, just swapped
    }
    else if (a->colliderType == ColliderType::BOX && b->colliderType == ColliderType::BOX) {
        OBB boxA = buildOBBFromBody(a);
        OBB boxB = buildOBBFromBody(b);
        result = checkBoxVsBox(boxA, boxB);
    }
    else if (a->colliderType == ColliderType::CAPSULE && b->colliderType == ColliderType::CAPSULE) {
        Capsule capA = buildCapsuleFromBody(a);
        Capsule capB = buildCapsuleFromBody(b);
        result = checkCapsuleVsCapsule(capA, capB);
    }
    else if (a->colliderType == ColliderType::CAPSULE && b->colliderType == ColliderType::SPHERE) {
        Capsule cap = buildCapsuleFromBody(a);
        Sphere sph = buildSphereFromBody(b);
        result = checkCapsuleVsSphere(cap, sph);
    }
    else if (a->colliderType == ColliderType::SPHERE && b->colliderType == ColliderType::CAPSULE) {
        Sphere sph = buildSphereFromBody(a);
        Capsule cap = buildCapsuleFromBody(b);
        result = checkCapsuleVsSphere(cap, sph);
    }
    else if (a->colliderType == ColliderType::CAPSULE && b->colliderType == ColliderType::BOX) {
        Capsule cap = buildCapsuleFromBody(a);
        OBB box = buildOBBFromBody(b);
        result = checkCapsuleVsBox(cap, box);
    }
    else if (a->colliderType == ColliderType::BOX && b->colliderType == ColliderType::CAPSULE) {
        OBB box = buildOBBFromBody(a);
        Capsule cap = buildCapsuleFromBody(b);
        result = checkCapsuleVsBox(cap, box);
    }
    else {
        // Default to OBB vs OBB for other combinations
        OBB boxA = buildOBBFromBody(a);
        OBB boxB = buildOBBFromBody(b);
        result = checkBoxVsBox(boxA, boxB);
    }

    return result;
}

CollisionResult PhysicsWorld::checkSphereVsSphere(const Sphere& a, const Sphere& b) const
{
    CollisionResult result;

    float dist = glm::distance(a.center, b.center);
    float sumRadius = a.radius + b.radius;

    if (dist < sumRadius) {
        result.collided = true;
        result.penetration = sumRadius - dist;

        if (dist > 0.0f) {
            result.normal = glm::normalize(b.center - a.center);
        } else {
            result.normal = glm::vec3(1.0f, 0.0f, 0.0f); // arbitrary normal if centers coincide
        }

        result.contactPoint = a.center + result.normal * a.radius;
    }

    return result;
}

CollisionResult PhysicsWorld::checkBoxVsSphere(const OBB& box, const Sphere& sphere) const
{
    CollisionResult result;

    // Find closest point on box to sphere center
    glm::vec3 closestPoint = sphere.center;

    // Project point onto box boundaries
    glm::vec3 boxMin = box.c - box.half;
    glm::vec3 boxMax = box.c + box.half;

    closestPoint.x = std::max(boxMin.x, std::min(closestPoint.x, boxMax.x));
    closestPoint.y = std::max(boxMin.y, std::min(closestPoint.y, boxMax.y));
    closestPoint.z = std::max(boxMin.z, std::min(closestPoint.z, boxMax.z));

    float dist = glm::distance(closestPoint, sphere.center);

    if (dist < sphere.radius) {
        result.collided = true;
        result.penetration = sphere.radius - dist;

        if (dist > 0.0f) {
            result.normal = glm::normalize(sphere.center - closestPoint);
        } else {
            // Sphere center is inside box, use face normal
            glm::vec3 centerToCenter = box.c - sphere.center;
            result.normal = glm::normalize(centerToCenter);
        }

        result.contactPoint = closestPoint;
    }

    return result;
}

CollisionResult PhysicsWorld::checkBoxVsBox(const OBB& a, const OBB& b) const
{
    // Use the existing SAT implementation
    float penetration;
    glm::vec3 normal;

    if (obbOverlapAndPenetration(a, b, penetration, normal)) {
        CollisionResult result;
        result.collided = true;
        result.penetration = penetration;
        result.normal = normal;
        result.contactPoint = (a.c + b.c) * 0.5f;
        return result;
    }

    return CollisionResult{}; // Return empty result if no collision
}

CollisionResult PhysicsWorld::checkCapsuleVsCapsule(const Capsule& a, const Capsule& b) const
{
    CollisionResult result;

    // Simplified capsule-capsule collision using center distance
    // A capsule is defined by a line segment and a radius
    
    // For now, use a simplified approach
    float minDist = glm::distance(a.center, b.center) - (a.radius + b.radius);

    if (minDist < 0.0f) {
        result.collided = true;
        result.penetration = -(minDist);
        result.normal = glm::normalize(b.center - a.center);
        result.contactPoint = a.center + result.normal * a.radius;
    }

    return result;
}

CollisionResult PhysicsWorld::checkCapsuleVsSphere(const Capsule& cap, const Sphere& sph) const
{
    CollisionResult result;

    // Find closest point on capsule line segment to sphere center
    glm::vec3 capStart = cap.center - cap.axis * (cap.height * 0.5f);
    glm::vec3 capEnd = cap.center + cap.axis * (cap.height * 0.5f);

    // Find closest point on line segment to sphere center
    glm::vec3 segmentVec = capEnd - capStart;
    float segmentLenSq = glm::dot(segmentVec, segmentVec);

    if (segmentLenSq < 1e-6f) {
        // Capsule is essentially a sphere
        Sphere capSphere;
        capSphere.center = cap.center;
        capSphere.radius = cap.radius;
        return checkSphereVsSphere(capSphere, sph);
    }

    float t = glm::dot(sph.center - capStart, segmentVec) / segmentLenSq;
    t = std::clamp(t, 0.0f, 1.0f);

    glm::vec3 closestOnSegment = capStart + segmentVec * t;
    float dist = glm::distance(closestOnSegment, sph.center);
    float combinedRadius = cap.radius + sph.radius;

    if (dist < combinedRadius) {
        result.collided = true;
        result.penetration = combinedRadius - dist;

        if (dist > 0.0f) {
            result.normal = glm::normalize(sph.center - closestOnSegment);
        } else {
            result.normal = glm::vec3(1.0f, 0.0f, 0.0f); // arbitrary
        }

        result.contactPoint = closestOnSegment + result.normal * cap.radius;
    }

    return result;
}

CollisionResult PhysicsWorld::checkCapsuleVsBox(const Capsule& cap, const OBB& box) const
{
    CollisionResult result;

    // This is a complex collision test that would require more sophisticated algorithms
    // For now, we'll approximate by sampling points along the capsule and testing against the box
    // A full implementation would use GJK or EPA algorithms

    // Simplified approach: treat capsule as a rounded line segment
    glm::vec3 capStart = cap.center - cap.axis * (cap.height * 0.5f);
    glm::vec3 capEnd = cap.center + cap.axis * (cap.height * 0.5f);

    // Test if either end of the capsule is inside the box
    glm::vec3 boxMin = box.c - box.half;
    glm::vec3 boxMax = box.c + box.half;

    bool startInside = (capStart.x >= boxMin.x && capStart.x <= boxMax.x &&
                        capStart.y >= boxMin.y && capStart.y <= boxMax.y &&
                        capStart.z >= boxMin.z && capStart.z <= boxMax.z);

    bool endInside = (capEnd.x >= boxMin.x && capEnd.x <= boxMax.x &&
                      capEnd.y >= boxMin.y && capEnd.y <= boxMax.y &&
                      capEnd.z >= boxMin.z && capEnd.z <= boxMax.z);

    if (startInside || endInside) {
        result.collided = true;
        result.penetration = cap.radius;
        result.normal = glm::vec3(0.0f, 1.0f, 0.0f); // arbitrary
        result.contactPoint = startInside ? capStart : capEnd;
        return result;
    }

    // More complex implementation would go here
    return result;
}

// -------------------- Constraints --------------------
void PhysicsWorld::addConstraint(Constraint* constraint)
{
    constraints.push_back(constraint);
}

void PhysicsWorld::clearConstraints()
{
    constraints.clear();
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

// -------------------- Resolve contact (deprecated - use resolveContactAdvanced) --------------------
// This function is kept for backward compatibility but is no longer used
// static void resolveContact(std::shared_ptr<RigidBody>& a,
//                            std::shared_ptr<RigidBody>& b,
//                            const glm::vec3& normal,
//                            float penetration)
// {
//     // Deprecated - see resolveContactAdvanced for PBR-aware collision response
// }

// Add a fluid volume to the physics world
void PhysicsWorld::addFluidVolume(const FluidVolume& fluid) {
    fluidVolumes.push_back(fluid);
}

// Check if a point is inside any fluid volume
bool PhysicsWorld::isInFluid(const glm::vec3& point, FluidVolume& outFluid) const {
    for (const auto& fluid : fluidVolumes) {
        if (point.x >= fluid.minBounds.x && point.x <= fluid.maxBounds.x &&
            point.y >= fluid.minBounds.y && point.y <= fluid.maxBounds.y &&
            point.z >= fluid.minBounds.z && point.z <= fluid.maxBounds.z) {
            outFluid = fluid;
            return true;
        }
    }
    return false;
}

// Advanced collision response considering PBR material properties
void PhysicsWorld::resolveContactAdvanced(std::shared_ptr<RigidBody>& a,
                                         std::shared_ptr<RigidBody>& b,
                                         const glm::vec3& normal,
                                         float penetration,
                                         const glm::vec3& contactPoint,
                                         float subdt)
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

    // --- Restitution based on PBR properties ---
    // Using metallic property to influence bounciness (higher metallic = more bouncy)
    float e = (a->restitution * a->metallic + b->restitution * b->metallic) * 0.5f;
    
    // If one body immovable and normal is mostly vertical, use material properties for bounce
    if ((aImmovable || bImmovable) && std::abs(normal.y) > 0.5f) {
        // Use roughness to dampen bounce (rougher surfaces = less bounce)
        e = e * (1.0f - (a->roughness + b->roughness) * 0.5f);
    }

    // --- Impulse ---
    float j = -(1.0f + e) * velAlongNormal / totalInvMass;
    glm::vec3 impulse = j * normal;

    if (!aImmovable) a->velocity -= impulse * invMassA;
    if (!bImmovable) b->velocity += impulse * invMassB;

    // --- Friction based on PBR properties ---
    glm::vec3 tangent = relVel - glm::dot(relVel, normal) * normal;
    if (glm::length2(tangent) > 1e-6f) {
        tangent = glm::normalize(tangent);
        float jt = -glm::dot(relVel, tangent) / totalInvMass;
        
        // Use both static and dynamic friction coefficients
        float frictionCoeff = std::sqrt(a->staticFriction * b->staticFriction);
        
        // Adjust friction based on roughness (rougher = more friction)
        frictionCoeff *= (1.0f + (a->roughness + b->roughness) * 0.5f);
        
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

            // -------- FLUID DYNAMICS --------
            // Check if body is in a fluid volume
            FluidVolume fluid;
            if (isInFluid(b->position, fluid)) {
                // Calculate buoyancy force
                glm::vec3 buoyancyForce = -gravity * b->buoyancyFactor * fluid.density * b->mass;
                
                // Calculate drag force
                glm::vec3 dragForce = -b->velocity * fluid.dragCoefficient * glm::length(b->velocity);
                
                // Calculate flow force if fluid has flow
                glm::vec3 flowForce = fluid.flowDirection * fluid.viscosity;
                
                // Apply fluid forces
                if (!b->isModel) {
                    b->applyForce(buoyancyForce);
                    b->applyForce(dragForce);
                    b->applyForce(flowForce);
                }
            }

            // -------- LINEAR --------
            if (!b->isModel) {
                if (glm::length2(b->forceAccumulator) > 0.0f) {
                    b->velocity += b->forceAccumulator * b->invMass() * subdt;
                }
                b->velocity += gravity * subdt;
            }

            // Apply damping
            b->velocity *= b->linearDamping;

            b->position += b->velocity * subdt;

            // -------- ANGULAR (QUATERNION) --------
            if (!b->isModel) {
                // Angular acceleration (world space)
                glm::vec3 angAccel = b->inertiaLocalInv * b->torqueAccumulator;

                b->angularVelocity += angAccel * subdt;

                // Apply angular damping
                b->angularVelocity *= b->angularDamping;

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

        // --- constraint solving ---
        for (auto& constraint : constraints) {
            constraint->preSolve(subdt);
        }

        // Solve constraints multiple times for stability
        for (int i = 0; i < 4; ++i) { // 4 iterations for constraint stability
            for (auto& constraint : constraints) {
                constraint->solve(*this, subdt);
            }
        }

        for (auto& constraint : constraints) {
            constraint->postSolve(subdt);
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

            // Use the new collision detection system
            CollisionResult collision = checkCollision(A, B);
            if (collision.collided) {
                // Use advanced collision response with PBR properties
                resolveContactAdvanced(A, B, collision.normal, collision.penetration, 
                                      collision.contactPoint, subdt);
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
                // Use dynamic friction coefficient
                float friction = b->dynamicFriction * 10.0f; // Scale for simulation
                b->velocity.x -= b->velocity.x * friction * subdt;
                b->velocity.z -= b->velocity.z * friction * subdt;

                if (glm::length2(glm::vec2(b->velocity.x, b->velocity.z)) < 1e-6f)
                    b->velocity.x = b->velocity.z = 0.0f;
            }
        }
    }
}