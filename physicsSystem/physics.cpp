#include "Physics.h"
#include "GJK.h"
#include <glm/gtx/norm.hpp>
#include <glm/gtx/string_cast.hpp>
#include <glm/gtx/intersect.hpp>
#include <algorithm>
#include <limits>
#include <cmath>
#include <sstream>
#include <iostream>
#include <unordered_map>

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

    for (const auto& b : bodies)
    {
        if (b.isStatic) continue;

        glm::vec3 min = b.position - b.scale * 0.5f;
        glm::vec3 max = b.position + b.scale * 0.5f;

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
    int hitIdx = -1;

    for (int i = 0; i < static_cast<int>(bodies.size()); ++i)
    {
        const auto& b = bodies[i];

        // Simple AABB-ray intersection test
        glm::vec3 min = b.position - b.scale * 0.5f;
        glm::vec3 max = b.position + b.scale * 0.5f;

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

                hitIdx = i;
                hit = true;
            }
        }
    }

    // Return as shared_ptr for API compatibility
    if (hit && hitIdx >= 0) {
        hitBody = std::make_shared<RigidBody>(bodies[hitIdx]);
    }

    return hit;
}

std::vector<BodyHandle> PhysicsWorld::getBodiesInAABB(const glm::vec3& min, const glm::vec3& max) const
{
    std::vector<BodyHandle> result;

    for (int i = 0; i < static_cast<int>(bodies.size()); ++i)
    {
        const auto& body = bodies[i];

        glm::vec3 bodyMin = body.position - body.scale * 0.5f;
        glm::vec3 bodyMax = body.position + body.scale * 0.5f;

        // Check if AABBs overlap
        if (bodyMin.x <= max.x && bodyMax.x >= min.x &&
            bodyMin.y <= max.y && bodyMax.y >= min.y &&
            bodyMin.z <= max.z && bodyMax.z >= min.z)
        {
            result.push_back(BodyHandle{i});
        }
    }

    return result;
}

BodyHandle PhysicsWorld::getBodyAtPoint(const glm::vec3& point, float radius) const
{
    for (int i = 0; i < static_cast<int>(bodies.size()); ++i)
    {
        const auto& body = bodies[i];

        float dist = glm::distance(point, body.position);
        if (dist <= radius)
        {
            return BodyHandle{i};
        }
    }

    return BodyHandle{-1};
}

// -------------------- Shape building (index-based) --------------------
Sphere PhysicsWorld::buildSphereFromIndex(int idx) const
{
    const RigidBody& rb = bodies[idx];
    Sphere sphere;
    sphere.center = rb.position;
    sphere.radius = (rb.scale.x + rb.scale.y + rb.scale.z) / 3.0f;
    return sphere;
}

Capsule PhysicsWorld::buildCapsuleFromIndex(int idx) const
{
    const RigidBody& rb = bodies[idx];
    Capsule capsule;
    capsule.center = rb.position;
    capsule.radius = rb.scale.x;
    capsule.height = rb.scale.y;
    capsule.axis = glm::vec3(0.0f, 1.0f, 0.0f);
    return capsule;
}

// -------------------- Collision Detection (index-based) --------------------
CollisionResult PhysicsWorld::checkCollision(int a, int b) const
{
    CollisionResult result;

    const RigidBody& bodyA = bodies[a];
    const RigidBody& bodyB = bodies[b];

    // Determine collision based on collider types
    if (bodyA.colliderType == ColliderType::SPHERE && bodyB.colliderType == ColliderType::SPHERE) {
        Sphere sa = buildSphereFromIndex(a);
        Sphere sb = buildSphereFromIndex(b);
        result = checkSphereVsSphere(sa, sb);
    }
    else if (bodyA.colliderType == ColliderType::BOX && bodyB.colliderType == ColliderType::SPHERE) {
        OBB box = buildOBBFromIndex(a);
        Sphere sph = buildSphereFromIndex(b);
        result = checkBoxVsSphere(box, sph);
    }
    else if (bodyA.colliderType == ColliderType::SPHERE && bodyB.colliderType == ColliderType::BOX) {
        Sphere sph = buildSphereFromIndex(a);
        OBB box = buildOBBFromIndex(b);
        result = checkBoxVsSphere(box, sph); // Same as above, just swapped
    }
    else if (bodyA.colliderType == ColliderType::BOX && bodyB.colliderType == ColliderType::BOX) {
        OBB boxA = buildOBBFromIndex(a);
        OBB boxB = buildOBBFromIndex(b);
        result = checkBoxVsBox(boxA, boxB);
    }
    else if (bodyA.colliderType == ColliderType::CAPSULE && bodyB.colliderType == ColliderType::CAPSULE) {
        Capsule capA = buildCapsuleFromIndex(a);
        Capsule capB = buildCapsuleFromIndex(b);
        result = checkCapsuleVsCapsule(capA, capB);
    }
    else if (bodyA.colliderType == ColliderType::CAPSULE && bodyB.colliderType == ColliderType::SPHERE) {
        Capsule cap = buildCapsuleFromIndex(a);
        Sphere sph = buildSphereFromIndex(b);
        result = checkCapsuleVsSphere(cap, sph);
    }
    else if (bodyA.colliderType == ColliderType::SPHERE && bodyB.colliderType == ColliderType::CAPSULE) {
        Sphere sph = buildSphereFromIndex(a);
        Capsule cap = buildCapsuleFromIndex(b);
        result = checkCapsuleVsSphere(cap, sph);
    }
    else if (bodyA.colliderType == ColliderType::CAPSULE && bodyB.colliderType == ColliderType::BOX) {
        Capsule cap = buildCapsuleFromIndex(a);
        OBB box = buildOBBFromIndex(b);
        result = checkCapsuleVsBox(cap, box);
    }
    else if (bodyA.colliderType == ColliderType::BOX && bodyB.colliderType == ColliderType::CAPSULE) {
        OBB box = buildOBBFromIndex(a);
        Capsule cap = buildCapsuleFromIndex(b);
        result = checkCapsuleVsBox(cap, box);
    }
    else {
        // MESH collider or unknown combination -> use GJK/EPA
        if (bodyA.colliderType == ColliderType::MESH || bodyB.colliderType == ColliderType::MESH) {
            result = checkGJKCollision(a, b);
        } else {
            // Default to OBB vs OBB for other combinations
            OBB boxA = buildOBBFromIndex(a);
            OBB boxB = buildOBBFromIndex(b);
            result = checkBoxVsBox(boxA, boxB);
        }
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

    CollisionResult result;
    return result;
}

// -------------------- GJK/EPA Collision for Mesh and Arbitrary Convex Shapes --------------------

/**
 * Extract vertices from a body for GJK support function.
 * For BOX: generates 8 corner vertices.
 * For SPHERE: generates approximating polyhedron vertices.
 * For CAPSULE: generates cylinder+hemisphere vertices.
 * For MESH: extracts mesh vertices (placeholder - needs mesh data).
 */
bool PhysicsWorld::extractMeshVertices(int bodyIdx, std::vector<glm::vec3>& outVertices, glm::mat4& outTransform) const {
    if (bodyIdx < 0 || bodyIdx >= static_cast<int>(bodies.size())) return false;

    const auto& b = bodies[bodyIdx];

    // Build world transform from position and rotation
    glm::mat4 rotation = glm::mat4_cast(b.rotation);
    glm::mat4 translation = glm::translate(glm::mat4(1.0f), b.position);
    outTransform = translation * rotation;

    switch (b.colliderType) {
        case ColliderType::BOX: {
            // 8 corners of the box
            glm::vec3 h = b.scale * 0.5f;
            outVertices = {
                {-h.x, -h.y, -h.z}, { h.x, -h.y, -h.z}, { h.x,  h.y, -h.z}, {-h.x,  h.y, -h.z},
                {-h.x, -h.y,  h.z}, { h.x, -h.y,  h.z}, { h.x,  h.y,  h.z}, {-h.x,  h.y,  h.z}
            };
            return true;
        }
        case ColliderType::SPHERE: {
            // Approximate sphere with icosahedron vertices (12 vertices)
            float r = b.scale.x;
            float t = (1.0f + std::sqrt(5.0f)) / 2.0f; // golden ratio
            outVertices = {
                {-r,  t*r, 0}, { r,  t*r, 0}, {-r, -t*r, 0}, { r, -t*r, 0},
                {0, -r,  t*r}, {0,  r,  t*r}, {0, -r, -t*r}, {0,  r, -t*r},
                { t*r, 0, -r}, { t*r, 0,  r}, {-t*r, 0, -r}, {-t*r, 0,  r}
            };
            // Normalize to sphere radius
            for (auto& v : outVertices) {
                v = glm::normalize(v) * r;
            }
            return true;
        }
        case ColliderType::CAPSULE: {
            // Approximate capsule with vertices along cylinder + hemisphere
            float radius = b.scale.x;
            float height = b.scale.y * 0.5f;
            int segments = 8;
            for (int i = 0; i < segments; ++i) {
                float angle = (2.0f * 3.14159f * i) / segments;
                float cosA = std::cos(angle);
                float sinA = std::sin(angle);
                // Cylinder top
                outVertices.push_back({radius * cosA, height, radius * sinA});
                // Cylinder bottom
                outVertices.push_back({radius * cosA, -height, radius * sinA});
                // Top hemisphere
                outVertices.push_back({radius * cosA * 0.707f, height + radius * 0.707f, radius * sinA * 0.707f});
                // Bottom hemisphere
                outVertices.push_back({radius * cosA * 0.707f, -height - radius * 0.707f, radius * sinA * 0.707f});
            }
            return true;
        }
        case ColliderType::MESH: {
            // For MESH type, we'd need actual mesh vertex data
            // Placeholder: use bounding box vertices
            glm::vec3 h = b.scale * 0.5f;
            outVertices = {
                {-h.x, -h.y, -h.z}, { h.x, -h.y, -h.z}, { h.x,  h.y, -h.z}, {-h.x,  h.y, -h.z},
                {-h.x, -h.y,  h.z}, { h.x, -h.y,  h.z}, { h.x,  h.y,  h.z}, {-h.x,  h.y,  h.z}
            };
            return true;
        }
        default:
            return false;
    }
}

/**
 * GJK/EPA collision detection between two bodies.
 * Used for MESH colliders and as a unified collision path.
 */
CollisionResult PhysicsWorld::checkGJKCollision(int a, int b) const {
    CollisionResult result;

    std::vector<glm::vec3> verticesA, verticesB;
    glm::mat4 transformA, transformB;

    if (!extractMeshVertices(a, verticesA, transformA) ||
        !extractMeshVertices(b, verticesB, transformB)) {
        return result;
    }

    glm::vec3 contactNormal;
    float penetrationDepth;
    glm::vec3 contactPoint;

    auto supportA = [&verticesA, &transformA](const glm::vec3& dir) {
        return SupportPolyhedron(verticesA, transformA, dir);
    };

    auto supportB = [&verticesB, &transformB](const glm::vec3& dir) {
        return SupportPolyhedron(verticesB, transformB, dir);
    };

    GJKResult gjkResult = GJK_DetectContact(supportA, supportB, contactNormal, penetrationDepth, contactPoint);

    if (gjkResult.collided || penetrationDepth > 0.0f) {
        result.collided = true;
        result.normal = contactNormal;
        result.penetration = penetrationDepth;
        result.contactPoint = contactPoint;
    }

    return result;
}

CollisionResult PhysicsWorld::checkCapsuleVsCapsule(const Capsule& a, const Capsule& b) const
{
    CollisionResult result;

    // Full capsule-capsule collision using closest points on line segments
    // A capsule is defined by a line segment (center axis) and a radius
    
    // Calculate capsule line segment endpoints
    glm::vec3 aStart = a.center - a.axis * (a.height * 0.5f);
    glm::vec3 aEnd = a.center + a.axis * (a.height * 0.5f);
    glm::vec3 bStart = b.center - b.axis * (b.height * 0.5f);
    glm::vec3 bEnd = b.center + b.axis * (b.height * 0.5f);
    
    // Find closest points on line segments using full algorithm
    glm::vec3 u = aEnd - aStart;
    glm::vec3 v = bEnd - bStart;
    glm::vec3 w = aStart - bStart;
    
    float a_dot = glm::dot(u, u);
    float b_dot = glm::dot(v, v);
    float c_dot = glm::dot(u, v);
    float d_dot = glm::dot(u, w);
    float e_dot = glm::dot(v, w);
    
    float denom = a_dot * b_dot - c_dot * c_dot;
    float s, t;
    
    // Handle parallel segments
    if (denom < 1e-6f) {
        s = 0.0f;
        t = (b_dot > 0.0f) ? glm::clamp(-e_dot / b_dot, 0.0f, 1.0f) : 0.0f;
    } else {
        // Compute closest points using full formula
        s = glm::clamp((b_dot * d_dot - c_dot * e_dot) / denom, 0.0f, 1.0f);
        t = glm::clamp((a_dot * e_dot - c_dot * d_dot) / denom, 0.0f, 1.0f);
        
        // Check if we need to clamp to segment endpoints
        if (s < 0.0f) {
            s = 0.0f;
            t = glm::clamp(-e_dot / b_dot, 0.0f, 1.0f);
        } else if (s > 1.0f) {
            s = 1.0f;
            t = glm::clamp((c_dot - e_dot) / b_dot, 0.0f, 1.0f);
        }
        
        if (t < 0.0f) {
            t = 0.0f;
            s = glm::clamp(d_dot / a_dot, 0.0f, 1.0f);
        } else if (t > 1.0f) {
            t = 1.0f;
            s = glm::clamp((d_dot + c_dot) / a_dot, 0.0f, 1.0f);
        }
    }
    
    // Calculate closest points
    glm::vec3 closestA = aStart + s * u;
    glm::vec3 closestB = bStart + t * v;
    
    // Calculate distance between closest points
    glm::vec3 diff = closestA - closestB;
    float distSq = glm::dot(diff, diff);
    float radiusSum = a.radius + b.radius;
    
    // Check for collision
    if (distSq < radiusSum * radiusSum) {
        result.collided = true;
        float dist = glm::sqrt(distSq);
        result.penetration = radiusSum - dist;
        
        // Calculate contact normal
        if (dist > 1e-6f) {
            result.normal = diff / dist;
        } else {
            // Capsules are nearly touching at same point, use perpendicular axis
            result.normal = glm::normalize(glm::vec3(-u.y, u.x, 0.0f));
        }
        
        // Calculate contact point (midpoint between closest points)
        result.contactPoint = (closestA + closestB) * 0.5f;
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

    // Full capsule-box collision using closest point on box to capsule line segment
    // A capsule is defined by a line segment and a radius
    
    // Calculate capsule line segment endpoints in world space
    glm::vec3 capStart = cap.center - cap.axis * (cap.height * 0.5f);
    glm::vec3 capEnd = cap.center + cap.axis * (cap.height * 0.5f);
    
    // Find closest point on box to capsule line segment
    // First, transform capsule endpoints to box's local space
    glm::mat3 boxRot = glm::mat3(box.axis[0], box.axis[1], box.axis[2]);
    glm::mat3 invBoxRot = glm::transpose(boxRot);
    
    glm::vec3 localCapStart = invBoxRot * (capStart - box.c);
    glm::vec3 localCapEnd = invBoxRot * (capEnd - box.c);
    
    // Find closest point on line segment to box in local space
    glm::vec3 closestLocal;
    
    // Clamp to box bounds in local space
    for (int i = 0; i < 3; i++) {
        // Find closest point on line segment for this axis
        float t0 = (box.half[i] - localCapStart[i]) / (localCapEnd[i] - localCapStart[i]);
        float t1 = (-box.half[i] - localCapStart[i]) / (localCapEnd[i] - localCapStart[i]);
        
        // Check if line segment intersects box slab
        if (glm::abs(localCapStart[i]) <= box.half[i] && 
            glm::abs(localCapEnd[i]) <= box.half[i]) {
            // Both points inside slab, use midpoint
            closestLocal[i] = (localCapStart[i] + localCapEnd[i]) * 0.5f;
        } else {
            // Find closest point on segment to slab
            float t = glm::clamp((-localCapStart[i]) / (localCapEnd[i] - localCapStart[i]), 0.0f, 1.0f);
            float pointOnSeg = localCapStart[i] + t * (localCapEnd[i] - localCapStart[i]);
            closestLocal[i] = glm::clamp(pointOnSeg, -box.half[i], box.half[i]);
        }
    }
    
    // Transform closest point back to world space
    glm::vec3 closestWorld = box.c + boxRot * closestLocal;
    
    // Find closest point on capsule segment to closest point on box
    glm::vec3 segmentVec = capEnd - capStart;
    float segmentLenSq = glm::dot(segmentVec, segmentVec);
    
    glm::vec3 closestOnCapsule;
    if (segmentLenSq < 1e-6f) {
        // Capsule is essentially a sphere
        closestOnCapsule = cap.center;
    } else {
        float t = glm::dot(closestWorld - capStart, segmentVec) / segmentLenSq;
        t = glm::clamp(t, 0.0f, 1.0f);
        closestOnCapsule = capStart + segmentVec * t;
    }
    
    // Calculate distance between closest points
    glm::vec3 diff = closestOnCapsule - closestWorld;
    float distSq = glm::dot(diff, diff);
    
    // Check for collision
    if (distSq < cap.radius * cap.radius) {
        result.collided = true;
        float dist = glm::sqrt(distSq);
        result.penetration = cap.radius - dist;
        
        // Calculate contact normal
        if (dist > 1e-6f) {
            result.normal = diff / dist;
        } else {
            // Use box normal based on which face is closest
            glm::vec3 absLocal = glm::abs(closestLocal);
            int maxAxis = 0;
            float maxDist = glm::abs(absLocal[maxAxis] - box.half[maxAxis]);
            
            for (int i = 1; i < 3; i++) {
                float d = glm::abs(absLocal[i] - box.half[i]);
                if (d < maxDist) {
                    maxDist = d;
                    maxAxis = i;
                }
            }
            
            result.normal = box.axis[maxAxis] * (closestLocal[maxAxis] > 0 ? 1.0f : -1.0f);
        }
        
        // Calculate contact point
        result.contactPoint = closestWorld;
    }

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
OBB PhysicsWorld::buildOBBFromIndex(int idx) const {
    const RigidBody& rb = bodies[idx];
    OBB obb;
    obb.c = rb.position;
    obb.half = rb.scale * 0.5f;
    obb.axis[0] = glm::vec3(1.0f, 0.0f, 0.0f);
    obb.axis[1] = glm::vec3(0.0f, 1.0f, 0.0f);
    obb.axis[2] = glm::vec3(0.0f, 0.0f, 1.0f);
    return obb;
}

// -------------------- Ground check --------------------
bool PhysicsWorld::isGrounded(int bodyIdx, float probeDistance) {
    const RigidBody& body = bodies[bodyIdx];
    glm::vec3 bottom = body.position - glm::vec3(0.0f, body.scale.y * 0.5f, 0.0f);
    glm::vec3 probeEnd = bottom - glm::vec3(0.0f, probeDistance, 0.0f);

    // Check against floor first
    if (floor.enabled) {
        float floorY = floor.position.y;
        if (bottom.y >= floorY - probeDistance && bottom.y <= floorY + 0.05f) {
            glm::vec3 localXZ = bottom - floor.position;
            if (glm::abs(localXZ.x) <= floor.size.x && glm::abs(localXZ.z) <= floor.size.y) {
                return true;
            }
        }
    }

    for (int i = 0; i < static_cast<int>(bodies.size()); ++i) {
        if (i == bodyIdx) continue;
        const auto& other = bodies[i];
        if (!other.isStatic) continue;
        glm::vec3 otherMin = other.position - other.scale * 0.5f;
        glm::vec3 otherMax = other.position + other.scale * 0.5f;
        if (bottom.x < otherMax.x && bottom.x > otherMin.x &&
            bottom.z < otherMax.z && bottom.z > otherMin.z &&
            probeEnd.y <= otherMax.y && bottom.y >= otherMax.y)
            return true;

       glm::vec3 bMin = body.position - body.scale * 0.5f;
       glm::vec3 bMax = body.position + body.scale * 0.5f;
       glm::vec3 oMin = other.position - other.scale * 0.5f;
       glm::vec3 oMax = other.position + other.scale * 0.5f;

       bool xOverlap = (bMax.x > oMin.x) && (bMin.x < oMax.x);
       bool zOverlap = (bMax.z > oMin.z) && (bMin.z < oMax.z);
       bool yClose   = (bMin.y - oMax.y) < probeDistance && (bMin.y - oMax.y) > -probeDistance;

       if (xOverlap && zOverlap && yClose) return true;

    }
    return false;
}

// -------------------- Handle-based API (contiguous storage) --------------------
BodyHandle PhysicsWorld::addBody(RigidBody body) {
    bodies.push_back(std::move(body));
    spatialGridDirty = true;
    return BodyHandle{static_cast<int>(bodies.size() - 1)};
}

void PhysicsWorld::removeBody(BodyHandle handle) {
    if (handle.isValid() && handle.index < static_cast<int>(bodies.size())) {
        // Swap-remove changes index of moved body - grid must be rebuilt
        bodies[handle.index] = std::move(bodies.back());
        bodies.pop_back();
        spatialGridDirty = true;
    }
}

RigidBody* PhysicsWorld::getBody(BodyHandle handle) {
    if (handle.isValid() && handle.index < static_cast<int>(bodies.size())) {
        return &bodies[handle.index];
    }
    return nullptr;
}

const RigidBody* PhysicsWorld::getBody(BodyHandle handle) const {
    if (handle.isValid() && handle.index < static_cast<int>(bodies.size())) {
        return &bodies[handle.index];
    }
    return nullptr;
}

void PhysicsWorld::clear() { bodies.clear(); }

// Legacy shared_ptr compatibility wrappers
void PhysicsWorld::addBody(const std::shared_ptr<RigidBody>& body) {
    if (body) {
        bodies.push_back(std::move(*body));
    }
}

void PhysicsWorld::removeBody(const std::shared_ptr<RigidBody>& body) {
    if (!body) return;
    for (int i = static_cast<int>(bodies.size()) - 1; i >= 0; --i) {
        // Compare by position (since shared_ptr no longer points to same object)
        if (bodies[i].position == body->position && 
            bodies[i].colliderType == body->colliderType) {
            bodies[i] = std::move(bodies.back());
            bodies.pop_back();
            return;
        }
    }
}

// -------------------- Adaptive substep --------------------
int PhysicsWorld::computeAdaptiveSubsteps(float dt) const {
    float maxVel = 0.0f;
    float minSize = std::numeric_limits<float>::max();
    for (const auto& b : bodies) {
        if (b.isStatic) continue;
        float vel = glm::length(b.velocity) * (1.0f + b.restitution);
        maxVel = std::max(maxVel, vel);
        minSize = std::min({minSize, b.scale.x, b.scale.y, b.scale.z});
    }
    if (maxVel < 1e-6f) return 1;
    int substeps = std::ceil(maxVel * dt / (0.2f * minSize));
    return std::clamp(substeps, 1, 12);
}

// -------------------- 3D Spatial Hash Grid Broadphase --------------------

/**
 * Rebuild the spatial hash grid from all bodies.
 * Called when bodies are added/removed or when the grid is dirty.
 */
void PhysicsWorld::rebuildSpatialGrid() {
    spatialGrid.clear();
    spatialGridEntries.clear();

    // Estimate cell size from average body scale if not set
    if (spatialCellSize < 0.1f) {
        float avgScale = 0.0f;
        for (const auto& b : bodies) {
            avgScale += (b.scale.x + b.scale.y + b.scale.z) / 3.0f;
        }
        spatialCellSize = bodies.empty() ? 2.0f : (avgScale / bodies.size()) * 1.5f;
    }

    const float invCell = 1.0f / spatialCellSize;

    for (int i = 0; i < static_cast<int>(bodies.size()); ++i) {
        const auto& b = bodies[i];

        // Compute AABB of body
        float halfExtX = b.scale.x * 0.5f;
        float halfExtY = b.scale.y * 0.5f;
        float halfExtZ = b.scale.z * 0.5f;

        // Determine which cells this body occupies
        int minX = static_cast<int>(std::floor((b.position.x - halfExtX) * invCell));
        int maxX = static_cast<int>(std::floor((b.position.x + halfExtX) * invCell));
        int minY = static_cast<int>(std::floor((b.position.y - halfExtY) * invCell));
        int maxY = static_cast<int>(std::floor((b.position.y + halfExtY) * invCell));
        int minZ = static_cast<int>(std::floor((b.position.z - halfExtZ) * invCell));
        int maxZ = static_cast<int>(std::floor((b.position.z + halfExtZ) * invCell));

        for (int cx = minX; cx <= maxX; ++cx) {
            for (int cy = minY; cy <= maxY; ++cy) {
                for (int cz = minZ; cz <= maxZ; ++cz) {
                    uint64_t hash = HashCell(cx, cy, cz);
                    auto& cell = spatialGrid[hash];
                    cell.bodyIndices.push_back(i);
                    spatialGridEntries.emplace_back(hash, i);
                }
            }
        }
    }

    spatialGridDirty = false;
}

/**
 * Generate potential collision pairs using spatial hash grid.
 * Bodies in the same cell or adjacent cells are potential pairs.
 * Uses full 3D AABB overlap test to reduce false positives.
 */
void PhysicsWorld::getPotentialPairs(std::vector<std::pair<int,int>>& outPairs) {
    outPairs.clear();
    if (bodies.size() < 2) return;

    // Rebuild grid if dirty
    if (spatialGridDirty) {
        rebuildSpatialGrid();
    }

    // Collect pairs from each cell, deduplicating with a sorted pair check
    // To avoid duplicate pairs, only generate (minIdx, maxIdx) where minIdx < maxIdx
    // and use a sorted set-like approach with a simple visited marker

    // Use a flat visited array instead of set for performance
    // We track which pairs we've already added using a sorted vector + binary search
    // For small-to-medium scenes, a simple O(n^2) per cell with dedup is fine
    // For large scenes, we use a bloom-like filter

    // Simple approach: iterate each cell, generate pairs, sort and unique
    std::vector<std::pair<int,int>> rawPairs;
    rawPairs.reserve(bodies.size() * 4); // estimate

    for (const auto& [cellHash, cell] : spatialGrid) {
        const auto& indices = cell.bodyIndices;
        for (size_t i = 0; i < indices.size(); ++i) {
            for (size_t j = i + 1; j < indices.size(); ++j) {
                int a = indices[i];
                int b = indices[j];
                if (a > b) { int tmp = a; a = b; b = tmp; }
                rawPairs.emplace_back(a, b);
            }
        }
    }

    if (rawPairs.empty()) return;

    // Sort and remove duplicates
    std::sort(rawPairs.begin(), rawPairs.end());
    rawPairs.erase(std::unique(rawPairs.begin(), rawPairs.end()), rawPairs.end());

    // Filter with 3D AABB overlap test (fast rejection before narrow phase)
    outPairs.reserve(rawPairs.size());
    for (const auto& [a, b] : rawPairs) {
        const auto& bodyA = bodies[a];
        const auto& bodyB = bodies[b];

        float ax = bodyA.scale.x * 0.5f, ay = bodyA.scale.y * 0.5f, az = bodyA.scale.z * 0.5f;
        float bx = bodyB.scale.x * 0.5f, by = bodyB.scale.y * 0.5f, bz = bodyB.scale.z * 0.5f;

        // 3D AABB overlap test
        if (std::abs(bodyA.position.x - bodyB.position.x) > ax + bx) continue;
        if (std::abs(bodyA.position.y - bodyB.position.y) > ay + by) continue;
        if (std::abs(bodyA.position.z - bodyB.position.z) > az + bz) continue;

        outPairs.emplace_back(a, b);
    }
}

// -------------------- Island-Based Sleeping System --------------------

int PhysicsWorld::findIsland(int i) {
    // Path compression for fast lookups
    while (islandRoot[i] != i) {
        islandRoot[i] = islandRoot[islandRoot[i]];
        i = islandRoot[i];
    }
    return i;
}

void PhysicsWorld::unionIslands(int i, int j) {
    int rootI = findIsland(i);
    int rootJ = findIsland(j);
    if (rootI == rootJ) return;

    // Union by rank
    if (islandRank[rootI] < islandRank[rootJ]) {
        islandRoot[rootI] = rootJ;
    } else if (islandRank[rootI] > islandRank[rootJ]) {
        islandRoot[rootJ] = rootI;
    } else {
        islandRoot[rootJ] = rootI;
        islandRank[rootI]++;
    }
}

void PhysicsWorld::wakeBody(int index) {
    if (index < 0 || index >= static_cast<int>(bodies.size())) return;
    auto& b = bodies[index];
    if (!b.isSleeping) return;

    b.isSleeping = false;
    b.sleepTime = 0.0f;

    // Wake entire island to prevent sleeping body from blocking active ones
    if (b.sleepIslandIndex >= 0 && b.sleepIslandIndex < static_cast<int>(sleepIslands.size())) {
        wakeIsland(b.sleepIslandIndex);
    }
}

void PhysicsWorld::wakeIsland(int islandIndex) {
    if (islandIndex < 0 || islandIndex >= static_cast<int>(sleepIslands.size())) return;
    for (int bodyIdx : sleepIslands[islandIndex]) {
        if (bodyIdx < static_cast<int>(bodies.size())) {
            bodies[bodyIdx].isSleeping = false;
            bodies[bodyIdx].sleepTime = 0.0f;
        }
    }
}

void PhysicsWorld::wakeAll() {
    for (auto& b : bodies) {
        b.isSleeping = false;
        b.sleepTime = 0.0f;
    }
    sleepIslands.clear();
}

void PhysicsWorld::buildIslands(const std::vector<std::pair<int,int>>& pairs) {
    int n = static_cast<int>(bodies.size());
    if (n == 0) {
        sleepIslands.clear();
        return;
    }

    // Initialize union-find
    islandRoot.resize(n);
    islandRank.assign(n, 0);
    for (int i = 0; i < n; ++i) {
        islandRoot[i] = i;
    }

    // Union bodies that are in contact
    for (const auto& [a, b] : pairs) {
        unionIslands(a, b);
    }

    // Build island groups
    sleepIslands.clear();
    std::unordered_map<int, int> rootToIsland;

    for (int i = 0; i < n; ++i) {
        int root = findIsland(i);
        auto it = rootToIsland.find(root);
        if (it == rootToIsland.end()) {
            int islandIdx = static_cast<int>(sleepIslands.size());
            rootToIsland[root] = islandIdx;
            sleepIslands.emplace_back();
            sleepIslands.back().push_back(i);
            bodies[i].sleepIslandIndex = islandIdx;
        } else {
            sleepIslands[it->second].push_back(i);
            bodies[i].sleepIslandIndex = it->second;
        }
    }
}

void PhysicsWorld::updateSleeping(float subdt) {
    // First, check for energy levels in each island
    // If ANY body in an island is active (above threshold), wake the whole island

    for (auto& island : sleepIslands) {
        bool anyActive = false;

        for (int bodyIdx : island) {
            if (bodyIdx < 0 || bodyIdx >= static_cast<int>(bodies.size())) continue;
            auto& b = bodies[bodyIdx];

            if (b.isStatic || b.isPlayer || b.isModel) continue;

            float linSpeed = glm::length2(b.velocity);
            float angSpeed = glm::length2(b.angularVelocity);
            float energy = linSpeed + angSpeed;

            float threshold2 = sleepVelocityThreshold * sleepVelocityThreshold +
                              sleepAngularVelocityThreshold * sleepAngularVelocityThreshold;

            if (energy > threshold2 || b.forceAccumulator != glm::vec3(0.0f)) {
                b.isSleeping = false;
                b.sleepTime = 0.0f;
                anyActive = true;
            }
        }

        if (!anyActive) {
            // All bodies below threshold - accumulate sleep time
            for (int bodyIdx : island) {
                if (bodyIdx < 0 || bodyIdx >= static_cast<int>(bodies.size())) continue;
                auto& b = bodies[bodyIdx];

                if (b.isStatic || b.isPlayer || b.isModel) continue;

                if (!b.isSleeping) {
                    b.sleepTime += subdt;
                    if (b.sleepTime >= sleepTimeRequired) {
                        b.isSleeping = true;
                        b.velocity = glm::vec3(0.0f);
                        b.angularVelocity = glm::vec3(0.0f);
                    }
                }
            }
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
void PhysicsWorld::resolveContactAdvanced(int a, int b,
                                         const glm::vec3& normal,
                                         float penetration,
                                         const glm::vec3& contactPoint,
                                         float subdt)
{
    RigidBody& bodyA = bodies[a];
    RigidBody& bodyB = bodies[b];

    bool aImmovable = bodyA.isStatic || bodyA.isModel;
    bool bImmovable = bodyB.isStatic || bodyB.isModel;

    float invMassA = aImmovable ? 0.0f : 1.0f / bodyA.mass;
    float invMassB = bImmovable ? 0.0f : 1.0f / bodyB.mass;
    float totalInvMass = invMassA + invMassB;

    if (totalInvMass < 1e-6f) return; // both immovable, skip

    // --- Position correction (penetration resolution) ---
    glm::vec3 correction = normal * penetration / totalInvMass * 0.8f; // 80% factor for stability
    if (!aImmovable) bodyA.position -= correction * invMassA;
    if (!bImmovable) bodyB.position += correction * invMassB;

    // --- Relative velocity along normal ---
    glm::vec3 relVel = bodyB.velocity - bodyA.velocity;
    float velAlongNormal = glm::dot(relVel, normal);

    // Bodies separating? Skip impulse
    if (velAlongNormal > 0.0f) return;

    // --- Restitution based on PBR properties ---
    // Using metallic property to influence bounciness (higher metallic = more bouncy)
    float e = (bodyA.restitution * bodyA.metallic + bodyB.restitution * bodyB.metallic) * 0.5f;
    
    // If one body immovable and normal is mostly vertical, use material properties for bounce
    if ((aImmovable || bImmovable) && std::abs(normal.y) > 0.5f) {
        // Use roughness to dampen bounce (rougher surfaces = less bounce)
        e = e * (1.0f - (bodyA.roughness + bodyB.roughness) * 0.5f);
    }

    // --- Impulse ---
    float j = -(1.0f + e) * velAlongNormal / totalInvMass;
    glm::vec3 impulse = j * normal;

    if (!aImmovable) bodyA.velocity -= impulse * invMassA;
    if (!bImmovable) bodyB.velocity += impulse * invMassB;

    // --- Friction based on PBR properties ---
    glm::vec3 tangent = relVel - glm::dot(relVel, normal) * normal;
    if (glm::length2(tangent) > 1e-6f) {
        tangent = glm::normalize(tangent);
        float jt = -glm::dot(relVel, tangent) / totalInvMass;
        
        // Use both static and dynamic friction coefficients
        float frictionCoeff = std::sqrt(bodyA.staticFriction * bodyB.staticFriction);
        
        // Adjust friction based on roughness (rougher = more friction)
        frictionCoeff *= (1.0f + (bodyA.roughness + bodyB.roughness) * 0.5f);
        
        float maxJt = j * frictionCoeff;
        jt = std::clamp(jt, -maxJt, maxJt);
        glm::vec3 frictionImpulse = jt * tangent;

        if (!aImmovable) bodyA.velocity -= frictionImpulse * invMassA;
        if (!bImmovable) bodyB.velocity += frictionImpulse * invMassB;
    }

    // --- Clamp tiny velocities ---
    if (!aImmovable && glm::length2(bodyA.velocity) < 1e-6f) bodyA.velocity = glm::vec3(0.0f);
    if (!bImmovable && glm::length2(bodyB.velocity) < 1e-6f) bodyB.velocity = glm::vec3(0.0f);

    // --- Zero rotation ---
    if (!aImmovable) bodyA.angularVelocity = glm::vec3(0.0f);
    if (!bImmovable) bodyB.angularVelocity = glm::vec3(0.0f);
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
            if (b.isStatic) continue;
            if (b.isSleeping) continue;  // Skip sleeping bodies

            // Save previous transforms
            b.prevPosition = b.position;
            b.prevRotation = b.rotation;

            // -------- FLUID DYNAMICS --------
            // Check if body is in a fluid volume
            FluidVolume fluid;
            if (isInFluid(b.position, fluid)) {
                // Calculate buoyancy force
                glm::vec3 buoyancyForce = -gravity * b.buoyancyFactor * fluid.density * b.mass;
                
                // Calculate drag force
                glm::vec3 dragForce = -b.velocity * fluid.dragCoefficient * glm::length(b.velocity);
                
                // Calculate flow force if fluid has flow
                glm::vec3 flowForce = fluid.flowDirection * fluid.viscosity;
                
                // Apply fluid forces
                if (!b.isModel) {
                    b.applyForce(buoyancyForce);
                    b.applyForce(dragForce);
                    b.applyForce(flowForce);
                }
            }

            // -------- LINEAR --------
            if (!b.isModel) {
                if (glm::length2(b.forceAccumulator) > 0.0f) {
                    b.velocity += b.forceAccumulator * b.invMass() * subdt;
                }
                b.velocity += gravity * subdt;
            }

            // Apply damping
            b.velocity *= b.linearDamping;

            b.position += b.velocity * subdt;

            // -------- ANGULAR (QUATERNION) --------
            if (!b.isModel) {
                // Angular acceleration (world space)
                glm::vec3 angAccel = b.inertiaLocalInv * b.torqueAccumulator;

                b.angularVelocity += angAccel * subdt;

                // Apply angular damping
                b.angularVelocity *= b.angularDamping;

                float angSpeed = glm::length(b.angularVelocity);
                if (angSpeed > 1e-5f) {
                    glm::vec3 axis = b.angularVelocity / angSpeed;
                    glm::quat dq = glm::angleAxis(angSpeed * subdt, axis);
                    b.rotation = glm::normalize(dq * b.rotation);
                }
            }

            // Clear accumulators
            b.forceAccumulator = glm::vec3(0.0f);
            b.torqueAccumulator = glm::vec3(0.0f);
        }

        // --- Floor collision resolution ---
        if (floor.enabled) {
            for (int i = 0; i < static_cast<int>(bodies.size()); ++i) {
                auto& b = bodies[i];
                if (b.isStatic) continue;

                // Calculate bottom of body
                float bottomY = b.position.y - b.scale.y * 0.5f;
                float floorY = floor.position.y;

                // Check if body is within floor bounds in XZ
                glm::vec3 localXZ = b.position - floor.position;
                bool withinBounds = (glm::abs(localXZ.x) <= floor.size.x + b.scale.x * 0.5f) &&
                                    (glm::abs(localXZ.z) <= floor.size.y + b.scale.z * 0.5f);

                if (withinBounds && bottomY <= floorY) {
                    // Penetration depth
                    float penetration = floorY - bottomY;

                    // Position correction - push body out of floor
                    b.position.y += penetration;

                    // Velocity reflection with restitution
                    if (b.velocity.y < 0.0f) {
                        b.velocity.y = -b.velocity.y * floor.restitution;

                        // If velocity is tiny, just zero it out (prevents micro-bouncing)
                        if (std::abs(b.velocity.y) < 0.01f) {
                            b.velocity.y = 0.0f;
                        }
                    }

                    // Apply floor friction
                    float friction = floor.friction;
                    b.velocity.x *= (1.0f - friction * subdt);
                    b.velocity.z *= (1.0f - friction * subdt);

                    // Mark as on ground
                    b.onGround = true;
                }
            }
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

            // Wake both bodies if either is sleeping (collision reactivates them)
            if (bodies[i].isSleeping) wakeBody(i);
            if (bodies[j].isSleeping) wakeBody(j);

            // Use the new collision detection system
            CollisionResult collision = checkCollision(i, j);
            if (collision.collided) {
                // Use advanced collision response with PBR properties
                resolveContactAdvanced(i, j, collision.normal, collision.penetration,
                                      collision.contactPoint, subdt);
            }
        }

        // --- build contact islands and update sleeping ---
        buildIslands(pairs);
        updateSleeping(subdt);

        // --- ground snap & friction ---
        for (int i = 0; i < static_cast<int>(bodies.size()); ++i) {
            auto& b = bodies[i];
            if (b.isStatic) continue;

            b.onGround = false;

            if (glm::length2(b.velocity) < 1e-6f)
                b.velocity = glm::vec3(0.0f);

            if (isGrounded(i, 1e-3f)) {
                b.velocity.y = 0.0f;
                b.onGround = true;
            }

            if (b.onGround && !b.isModel) {
                // Use dynamic friction coefficient
                float friction = b.dynamicFriction * 10.0f; // Scale for simulation
                b.velocity.x -= b.velocity.x * friction * subdt;
                b.velocity.z -= b.velocity.z * friction * subdt;

                if (glm::length2(glm::vec2(b.velocity.x, b.velocity.z)) < 1e-6f)
                    b.velocity.x = b.velocity.z = 0.0f;
            }
        }
    }
}