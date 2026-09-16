#include "GJK.h"
#include "Physics.h"  // For MAX_SUB_STEP_DT
#include <glm/gtx/norm.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <unordered_map>

// ============================================================================
// SUPPORT FUNCTIONS
// ============================================================================

SupportResult SupportPolyhedron(
    const std::vector<glm::vec3>& vertices,
    const glm::mat4& worldTransform,
    const glm::vec3& direction)
{
    SupportResult result;
    result.point = glm::vec3(0.0f);
    result.normal = glm::vec3(0.0f);
    result.vertexIndex = -1;
    
    if (vertices.empty()) return result;
    
    // Transform direction to local space
    glm::mat3 invTransform = glm::transpose(glm::mat3(worldTransform));
    glm::vec3 localDir = invTransform * direction;
    
    // Find farthest vertex in local space
    float maxDot = -std::numeric_limits<float>::max();
    int bestIndex = 0;
    
    for (size_t i = 0; i < vertices.size(); i++) {
        float dot = glm::dot(vertices[i], localDir);
        if (dot > maxDot) {
            maxDot = dot;
            bestIndex = static_cast<int>(i);
        }
    }
    
    // Transform back to world space
    result.point = glm::vec3(worldTransform * glm::vec4(vertices[bestIndex], 1.0f));
    result.vertexIndex = bestIndex;
    
    return result;
}

SupportResult SupportSphere(
    const glm::vec3& center,
    float radius,
    const glm::vec3& direction)
{
    SupportResult result;
    glm::vec3 normalizedDir = glm::normalize(direction);
    result.point = center + normalizedDir * radius;
    result.normal = normalizedDir;
    return result;
}

SupportResult SupportBox(
    const glm::vec3& center,
    const glm::vec3& halfExtents,
    const glm::mat4& rotation,
    const glm::vec3& direction)
{
    SupportResult result;
    
    // Transform direction to local space
    glm::mat3 rotMat = glm::mat3(rotation);
    glm::vec3 localDir = glm::transpose(rotMat) * direction;
    
    // Find farthest corner in local space
    glm::vec3 localPoint(
        localDir.x > 0 ? halfExtents.x : -halfExtents.x,
        localDir.y > 0 ? halfExtents.y : -halfExtents.y,
        localDir.z > 0 ? halfExtents.z : -halfExtents.z
    );
    
    // Transform back to world space
    result.point = center + rotMat * localPoint;
    
    return result;
}

SupportResult SupportCapsule(
    const glm::vec3& a,
    const glm::vec3& b,
    float radius,
    const glm::vec3& direction)
{
    SupportResult result;
    
    // Project direction onto line segment
    glm::vec3 ab = b - a;
    float t = glm::dot(direction, ab) / glm::dot(ab, ab);
    t = glm::clamp(t, 0.0f, 1.0f);
    
    // Closest point on line segment
    glm::vec3 closestPoint = a + t * ab;
    
    // Support point is in direction from closest point
    glm::vec3 normalizedDir = glm::normalize(direction);
    result.point = closestPoint + normalizedDir * radius;
    
    return result;
}

// ============================================================================
// GJK ALGORITHM
// ============================================================================

GJKResult GJK_Intersect(
    std::function<SupportResult(const glm::vec3&)> supportA,
    std::function<SupportResult(const glm::vec3&)> supportB,
    int maxIterations)
{
    GJKResult result;
    result.iterations = 0;
    
    Simplex simplex;
    
    // Initial search direction (arbitrary)
    glm::vec3 searchDir = glm::vec3(1.0f, 0.0f, 0.0f);
    
    // Get first support point
    SupportResult supA = supportA(searchDir);
    SupportResult supB = supportB(-searchDir);
    
    simplex.AddPoint(supA.point - supB.point, supA.point, supB.point, supA.vertexIndex);
    
    // Search direction points toward origin
    searchDir = -simplex.points[0];
    
    // Main GJK loop
    for (int iter = 0; iter < maxIterations; iter++) {
        result.iterations = iter;
        
        // Normalize search direction
        float len = glm::length(searchDir);
        if (len < 0.0001f) {
            // Search direction too small - shapes intersect
            result.collided = true;
            result.distance = 0.0f;
            result.closestPointA = simplex.pointsA[0];
            result.closestPointB = simplex.pointsB[0];
            return result;
        }
        
        searchDir = glm::normalize(searchDir);
        
        // Get support point in search direction
        supA = supportA(searchDir);
        supB = supportB(-searchDir);
        
        glm::vec3 newPoint = supA.point - supB.point;
        
        // Check if we've made progress
        float dot = glm::dot(newPoint, searchDir);
        if (dot < 0.0001f) {
            // No progress - shapes don't intersect
            // Find closest points on simplex to origin
            result.collided = false;
            result.distance = len;
            result.closestPointA = simplex.pointsA[0];
            result.closestPointB = simplex.pointsB[0];
            result.contactNormal = searchDir;
            return result;
        }
        
        // Add new point to simplex
        simplex.AddPoint(newPoint, supA.point, supB.point, supA.vertexIndex);
        
        // Check if origin is enclosed
        if (simplex.Size() == 4) {
            // Tetrahedron - check if origin is inside
            // For now, just say they intersect
            result.collided = true;
            result.distance = 0.0f;
            result.closestPointA = simplex.pointsA[0];
            result.closestPointB = simplex.pointsB[0];
            return result;
        }
        
        // Full GJK simplex reduction using proper case handling
        if (simplex.Size() > 1) {
            // Determine which Voronoi region contains the origin
            glm::vec3 a = simplex.points[0];
            glm::vec3 b = simplex.points[simplex.Size() - 1];
            
            if (simplex.Size() == 2) {
                // Line segment - find closest point to origin
                glm::vec3 ab = b - a;
                float t = -glm::dot(a, ab) / glm::dot(ab, ab);
                t = glm::clamp(t, 0.0f, 1.0f);
                searchDir = a + t * ab;
                
                // Keep both points for next iteration
                if (glm::length(searchDir) < 0.0001f) {
                    return GJKResult{true, 0.0f, a, b};  // Origin inside
                }
            }
            else if (simplex.Size() == 3) {
                // Triangle - check which edge region or face region
                glm::vec3 ab = b - a;
                glm::vec3 ao = -a;
                
                // Check edge regions
                float t = glm::dot(ao, ab) / glm::dot(ab, ab);
                if (t < 0.0f || t > 1.0f) {
                    // Origin not in AB edge region, check other edges
                    glm::vec3 ac = simplex.points[1] - a;
                    t = glm::dot(ao, ac) / glm::dot(ac, ac);
                    if (t >= 0.0f && t <= 1.0f) {
                        searchDir = a + t * ac;
                    } else {
                        searchDir = ao;
                    }
                } else {
                    searchDir = glm::normalize(glm::cross(glm::cross(ab, ao), ab));
                }
                
                if (glm::length(searchDir) < 0.0001f) {
                    return GJKResult{true, 0.0f, a, b};  // Origin inside
                }
            }
            else if (simplex.Size() == 4) {
                // Tetrahedron - origin is inside, collision detected
                return GJKResult{true, 0.0f, simplex.points[0], simplex.points[1]};
            }
            
            // Keep relevant points in simplex
            if (simplex.Size() > 1 && glm::length(searchDir) > 0.0001f) {
                Simplex newSimplex;
                // Keep points that define the search direction
                for (int i = 0; i < simplex.Size(); i++) {
                    if (glm::dot(simplex.points[i], searchDir) > 
                        glm::dot(simplex.points[0], searchDir) - 0.001f) {
                        newSimplex.AddPoint(
                            simplex.points[i],
                            simplex.pointsA[i],
                            simplex.pointsB[i],
                            simplex.indices[i]
                        );
                    }
                }
                if (newSimplex.Size() > 0) {
                    simplex = newSimplex;
                }
            }
        } else {
            searchDir = -simplex.points[0];
        }
    }
    
    // Max iterations reached
    result.collided = false;
    result.distance = glm::length(searchDir);
    return result;
}

bool GJK_Intersect_Fast(
    std::function<SupportResult(const glm::vec3&)> supportA,
    std::function<SupportResult(const glm::vec3&)> supportB,
    int maxIterations)
{
    Simplex simplex;
    glm::vec3 searchDir = glm::vec3(1.0f, 0.0f, 0.0f);
    
    SupportResult supA = supportA(searchDir);
    SupportResult supB = supportB(-searchDir);
    simplex.AddPoint(supA.point - supB.point, supA.point, supB.point);
    
    searchDir = -simplex.points[0];
    
    for (int iter = 0; iter < maxIterations; iter++) {
        float len = glm::length(searchDir);
        if (len < 0.0001f) return true;
        
        searchDir = glm::normalize(searchDir);
        
        supA = supportA(searchDir);
        supB = supportB(-searchDir);
        glm::vec3 newPoint = supA.point - supB.point;
        
        if (glm::dot(newPoint, searchDir) < 0.0001f) return false;
        
        simplex.AddPoint(newPoint, supA.point, supB.point);
        
        if (simplex.Size() == 4) return true;

        // Full simplex reduction with proper Voronoi region handling
        if (simplex.Size() == 2) {
            // Line segment case
            glm::vec3 a = simplex.points[0];
            glm::vec3 b = simplex.points[1];
            glm::vec3 ab = b - a;
            glm::vec3 ao = -a;
            
            float t = glm::dot(ao, ab) / glm::dot(ab, ab);
            if (t >= 0.0f && t <= 1.0f) {
                // Origin is in edge region
                searchDir = glm::normalize(glm::cross(glm::cross(ab, ao), ab));
            } else {
                // Origin is in vertex region
                searchDir = glm::normalize(ao);
            }
        }
        else if (simplex.Size() == 3) {
            // Triangle case
            glm::vec3 a = simplex.points[0];
            glm::vec3 b = simplex.points[1];
            glm::vec3 c = simplex.points[2];
            
            glm::vec3 ab = b - a;
            glm::vec3 ac = c - a;
            glm::vec3 ao = -a;
            
            // Compute triangle normal
            glm::vec3 abc = glm::normalize(glm::cross(ab, ac));
            
            // Check edge regions
            glm::vec3 abPerp = glm::cross(abc, ab);
            glm::vec3 acPerp = glm::cross(ac, abc);
            
            if (glm::dot(abPerp, ao) >= 0.0f && glm::dot(acPerp, ao) >= 0.0f) {
                // Origin is in face region
                searchDir = abc;
            } else if (glm::dot(ab, ao) >= 0.0f) {
                // AB edge region
                searchDir = glm::normalize(glm::cross(glm::cross(ab, ao), ab));
            } else if (glm::dot(ac, ao) >= 0.0f) {
                // AC edge region
                searchDir = glm::normalize(glm::cross(glm::cross(ac, ao), ac));
            } else {
                // A vertex region
                searchDir = glm::normalize(ao);
            }
        }
    }
    
    return false;
}

// ============================================================================
// EPA (EXPANDING POLYTOPE ALGORITHM)
// ============================================================================

/**
 * EPA - Expanding Polytope Algorithm
 * 
 * Takes the final GJK simplex (which contains the origin) and expands
 * it to find the closest face to the origin. That face's normal and
 * distance give us the penetration depth and contact normal.
 */
EPAResult EPA(
    const Simplex& simplex,
    std::function<SupportResult(const glm::vec3&)> supportA,
    std::function<SupportResult(const glm::vec3&)> supportB,
    int maxIterations)
{
    EPAResult result;
    result.iterations = 0;

    if (simplex.Size() < 4) {
        return result;  // Need at least a tetrahedron
    }

    // EPA polytope: list of triangular faces
    struct Face {
        int v[3];           // Vertex indices
        glm::vec3 normal;   // Face normal (points away from origin)
        float distance;     // Distance from origin to face plane
        bool valid;         // Face is still part of the polytope
    };

    // Vertices of the expanding polytope
    std::vector<glm::vec3> vertices;
    vertices.reserve(maxIterations + 4);

    // Initialize vertices from GJK simplex
    for (int i = 0; i < 4; ++i) {
        vertices.push_back(simplex.points[i]);
    }

    // Initialize faces of the tetrahedron
    // A tetrahedron has 4 faces. We need to orient them so normals point outward.
    std::vector<Face> faces;
    faces.reserve(maxIterations + 4);

    // Face orientation: use the centroid of the tetrahedron (not a single
    // reference vertex which can drift with degenerate simplices).
    glm::vec3 tetCentroid(0.0f);
    for (int i = 0; i < 4; ++i) tetCentroid += vertices[i];
    tetCentroid *= 0.25f;

    auto createFace = [&](int a, int b, int c, int /*ref*/) {
        Face f;
        f.v[0] = a; f.v[1] = b; f.v[2] = c;
        f.valid = true;

        glm::vec3 edge1 = vertices[b] - vertices[a];
        glm::vec3 edge2 = vertices[c] - vertices[a];
        f.normal = glm::cross(edge1, edge2);
        float len = glm::length(f.normal);

        if (len < 1e-8f) {
            f.valid = false;
            return f;
        }

        f.normal /= len;
        f.distance = glm::dot(f.normal, vertices[a]);

        // Ensure normal points outward from the tetrahedron centroid.
        // If the centroid is on the same side as the normal, flip.
        if (glm::dot(f.normal, tetCentroid - vertices[a]) > 0.0f) {
            f.normal = -f.normal;
            f.distance = -f.distance;
            std::swap(f.v[0], f.v[1]);
        }

        return f;
    };

    // Tetrahedron faces: (0,1,2) ref=3, (0,2,3) ref=1, (0,3,1) ref=2, (1,3,2) ref=0
    faces.push_back(createFace(0, 1, 2, 3));
    faces.push_back(createFace(0, 2, 3, 1));
    faces.push_back(createFace(0, 3, 1, 2));
    faces.push_back(createFace(1, 3, 2, 0));

    // EPA main loop: expand polytope toward the closest face
    for (int iter = 0; iter < maxIterations; ++iter) {
        result.iterations = iter + 1;

        // Find the face closest to the origin
        int closestFace = -1;
        float minDistance = std::numeric_limits<float>::max();

        for (int i = 0; i < static_cast<int>(faces.size()); ++i) {
            if (!faces[i].valid) continue;
            float d = std::abs(faces[i].distance);
            if (d < minDistance) {
                minDistance = d;
                closestFace = i;
            }
        }

        if (closestFace == -1) {
            break;  // No valid faces
        }

        // Get support point in direction of the closest face normal
        const Face& face = faces[closestFace];
        glm::vec3 dir = face.normal;
        float dirLen = glm::length(dir);
        if (dirLen < 1e-8f) continue;
        dir /= dirLen;

        SupportResult supA = supportA(dir);
        SupportResult supB = supportB(-dir);
        glm::vec3 supportPoint = supA.point - supB.point;

        // Check convergence: if support point is close enough to the face, we're done
        float supportDist = glm::dot(dir, supportPoint);
        float convergence = supportDist - std::abs(face.distance);

        if (convergence < 0.001f) {
            result.found = true;
            result.penetrationDepth = std::abs(face.distance);
            result.contactNormal = face.normal;

            // Contact point: midpoint between closest points on A and B
            result.contactPoint = (supA.point + supB.point) * 0.5f;
            return result;
        }

        // Add new vertex
        int newVertIdx = static_cast<int>(vertices.size());
        vertices.push_back(supportPoint);

        // Find horizon edges: edges of the closest face's visible region
        // An edge is on the horizon if the new vertex can "see" one face but not its neighbor
        std::vector<std::pair<int, int>> horizonEdges;
        std::vector<int> visibleFaces;

        // Mark faces visible from the new point
        for (int i = 0; i < static_cast<int>(faces.size()); ++i) {
            if (!faces[i].valid) continue;
            // If the new vertex is on the positive side of the face plane, it's visible
            glm::vec3 toNew = supportPoint - vertices[faces[i].v[0]];
            if (glm::dot(faces[i].normal, toNew) > 0.0f) {
                faces[i].valid = false;
                visibleFaces.push_back(i);
            }
        }

        // Find horizon edges: edges that belong to exactly one visible face
        for (int vi : visibleFaces) {
            const Face& f = faces[vi];
            for (int e = 0; e < 3; ++e) {
                int vStart = f.v[e];
                int vEnd = f.v[(e + 1) % 3];

                // Check if any adjacent (non-visible) face shares this edge
                bool shared = false;
                for (int j = 0; j < static_cast<int>(faces.size()); ++j) {
                    if (j == vi || !faces[j].valid) continue;
                    const Face& g = faces[j];
                    for (int ge = 0; ge < 3; ++ge) {
                        if (g.v[ge] == vEnd && g.v[(ge + 1) % 3] == vStart) {
                            shared = true;
                            break;
                        }
                    }
                    if (shared) break;
                }
                if (!shared) {
                    horizonEdges.emplace_back(vStart, vEnd);
                }
            }
        }

        // Create new faces from horizon edges to the new vertex
        for (const auto& edge : horizonEdges) {
            Face newFace;
            newFace.v[0] = edge.first;
            newFace.v[1] = edge.second;
            newFace.v[2] = newVertIdx;
            newFace.valid = true;

            glm::vec3 e1 = vertices[edge.second] - vertices[edge.first];
            glm::vec3 e2 = vertices[newVertIdx] - vertices[edge.first];
            newFace.normal = glm::cross(e1, e2);
            float len = glm::length(newFace.normal);
            if (len < 1e-8f) {
                newFace.valid = false;
                continue;
            }
            newFace.normal /= len;
            newFace.distance = glm::dot(newFace.normal, vertices[edge.first]);

            // Ensure normal points outward
            if (newFace.distance < 0.0f) {
                newFace.normal = -newFace.normal;
                newFace.distance = -newFace.distance;
                std::swap(newFace.v[0], newFace.v[1]);
            }

            faces.push_back(newFace);
        }
    }

    // If we ran out of iterations, use the closest face we found
    for (const auto& face : faces) {
        if (!face.valid) continue;
        float d = std::abs(face.distance);
        if (d < result.penetrationDepth || !result.found) {
            result.found = true;
            result.penetrationDepth = d;
            result.contactNormal = face.normal;
        }
    }

    return result;
}

/**
 * Combined GJK + EPA: detect collision and get contact info
 */
GJKResult GJK_DetectContact(
    std::function<SupportResult(const glm::vec3&)> supportA,
    std::function<SupportResult(const glm::vec3&)> supportB,
    glm::vec3& outContactNormal,
    float& outPenetrationDepth,
    glm::vec3& outContactPoint)
{
    GJKResult gjkResult = GJK_Intersect(supportA, supportB);

    if (gjkResult.collided && gjkResult.distance < 0.001f) {
        // Shapes are intersecting - use EPA for penetration info
        Simplex simplex;
        if (GJK_Intersect_WithSimplex(supportA, supportB, simplex, 100)) {
            EPAResult epaResult = EPA(simplex, supportA, supportB, 50);

            if (epaResult.found) {
                outContactNormal = epaResult.contactNormal;
                outPenetrationDepth = epaResult.penetrationDepth;
                outContactPoint = epaResult.contactPoint;
            } else {
                // Fallback: use GJK normal
                outContactNormal = gjkResult.contactNormal;
                outPenetrationDepth = 0.01f;
                outContactPoint = (gjkResult.closestPointA + gjkResult.closestPointB) * 0.5f;
            }
        } else {
            outContactNormal = gjkResult.contactNormal;
            outPenetrationDepth = 0.01f;
            outContactPoint = (gjkResult.closestPointA + gjkResult.closestPointB) * 0.5f;
        }
    } else if (!gjkResult.collided) {
        outContactNormal = gjkResult.contactNormal;
        outPenetrationDepth = 0.0f;
        outContactPoint = gjkResult.closestPointA;
    }

    return gjkResult;
}

/**
 * GJK with simplex output (needed for EPA)
 */
bool GJK_Intersect_WithSimplex(
    std::function<SupportResult(const glm::vec3&)> supportA,
    std::function<SupportResult(const glm::vec3&)> supportB,
    Simplex& outSimplex,
    int maxIterations)
{
    outSimplex.Clear();

    glm::vec3 searchDir = glm::vec3(1.0f, 0.0f, 0.0f);

    SupportResult supA = supportA(searchDir);
    SupportResult supB = supportB(-searchDir);

    outSimplex.AddPoint(supA.point - supB.point, supA.point, supB.point, supA.vertexIndex);

    searchDir = -supA.point + supB.point;

    for (int iter = 0; iter < maxIterations; ++iter) {
        if (glm::length2(searchDir) < 1e-8f) {
            searchDir = glm::vec3(1.0f, 0.0f, 0.0f);
        }

        supA = supportA(searchDir);
        supB = supportB(-searchDir);

        glm::vec3 newPoint = supA.point - supB.point;

        if (glm::dot(newPoint, searchDir) <= 0.0f) {
            return false;  // No collision
        }

        outSimplex.AddPoint(newPoint, supA.point, supB.point, supA.vertexIndex);

        switch (outSimplex.Size()) {
            case 2: {
                glm::vec3 a = outSimplex.points[1];
                glm::vec3 b = outSimplex.points[0];
                glm::vec3 ab = b - a;
                glm::vec3 ao = -a;

                glm::vec3 abPerp = glm::cross(ab, glm::cross(ao, ab));
                float len = glm::length(abPerp);
                searchDir = (len > 1e-8f) ? abPerp : glm::cross(ab, glm::vec3(1,0,0));
                break;
            }
            case 3: {
                glm::vec3 a = outSimplex.points[2];
                glm::vec3 b = outSimplex.points[1];
                glm::vec3 c = outSimplex.points[0];
                glm::vec3 ab = b - a;
                glm::vec3 ac = c - a;
                glm::vec3 ao = -a;

                glm::vec3 abc = glm::cross(ab, ac);
                glm::vec3 abPerp = glm::cross(ab, abc);
                glm::vec3 acPerp = glm::cross(abc, ac);

                float abDot = glm::dot(abPerp, ao);
                float acDot = glm::dot(acPerp, ao);

                if (abDot > 0.0f && abDot > glm::dot(ab, ao)) {
                    outSimplex.points = {a, b};
                    outSimplex.pointsA = {outSimplex.pointsA[2], outSimplex.pointsA[1]};
                    outSimplex.pointsB = {outSimplex.pointsB[2], outSimplex.pointsB[1]};
                    glm::vec3 abEdge = b - a;
                    glm::vec3 abPerp2 = glm::cross(abEdge, glm::cross(ao, abEdge));
                    float len2 = glm::length(abPerp2);
                    searchDir = (len2 > 1e-8f) ? abPerp2 : glm::cross(abEdge, glm::vec3(1,0,0));
                } else if (acDot > 0.0f && acDot > glm::dot(ac, ao)) {
                    outSimplex.points = {a, c};
                    outSimplex.pointsA = {outSimplex.pointsA[2], outSimplex.pointsA[0]};
                    outSimplex.pointsB = {outSimplex.pointsB[2], outSimplex.pointsB[0]};
                    glm::vec3 acEdge = c - a;
                    glm::vec3 acPerp2 = glm::cross(glm::cross(ao, acEdge), acEdge);
                    float len2 = glm::length(acPerp2);
                    searchDir = (len2 > 1e-8f) ? acPerp2 : glm::cross(acEdge, glm::vec3(1,0,0));
                } else {
                    float abcLen = glm::length(abc);
                    float abcDot = glm::dot(abc, ao);
                    searchDir = (abcDot > 0.0f) ? (abc / abcLen) : (-abc / abcLen);
                }
                break;
            }
            case 4: {
                glm::vec3 a = outSimplex.points[3];
                glm::vec3 b = outSimplex.points[2];
                glm::vec3 c = outSimplex.points[1];
                glm::vec3 d = outSimplex.points[0];

                glm::vec3 ab = b - a;
                glm::vec3 ac = c - a;
                glm::vec3 ad = d - a;
                glm::vec3 ao = -a;

                glm::vec3 abc = glm::cross(ab, ac);
                glm::vec3 acd = glm::cross(ac, ad);
                glm::vec3 adb = glm::cross(ad, ab);

                float abcDot = glm::dot(abc, ao);
                float acdDot = glm::dot(acd, ao);
                float adbDot = glm::dot(adb, ao);

                if (abcDot > 0.0f) {
                    outSimplex.points = {a, b, c};
                    outSimplex.pointsA = {outSimplex.pointsA[3], outSimplex.pointsA[2], outSimplex.pointsA[1]};
                    outSimplex.pointsB = {outSimplex.pointsB[3], outSimplex.pointsB[2], outSimplex.pointsB[1]};
                    float abcLen = glm::length(abc);
                    searchDir = (abcDot > 0.0f) ? (abc / abcLen) : (-abc / abcLen);
                } else if (acdDot > 0.0f) {
                    outSimplex.points = {a, c, d};
                    outSimplex.pointsA = {outSimplex.pointsA[3], outSimplex.pointsA[1], outSimplex.pointsA[0]};
                    outSimplex.pointsB = {outSimplex.pointsB[3], outSimplex.pointsB[1], outSimplex.pointsB[0]};
                    float acdLen = glm::length(acd);
                    searchDir = (acdDot > 0.0f) ? (acd / acdLen) : (-acd / acdLen);
                } else if (adbDot > 0.0f) {
                    outSimplex.points = {a, d, b};
                    outSimplex.pointsA = {outSimplex.pointsA[3], outSimplex.pointsA[0], outSimplex.pointsA[2]};
                    outSimplex.pointsB = {outSimplex.pointsB[3], outSimplex.pointsB[0], outSimplex.pointsB[2]};
                    float adbLen = glm::length(adb);
                    searchDir = (adbDot > 0.0f) ? (adb / adbLen) : (-adb / adbLen);
                } else {
                    return true;  // Origin is inside tetrahedron = collision
                }
                break;
            }
        }
    }

    return false;
}

// ============================================================================
// CONVENIENCE FUNCTIONS
// ============================================================================

GJKResult GJK_CheckCollision(
    const std::vector<glm::vec3>& verticesA,
    const glm::mat4& transformA,
    const std::vector<glm::vec3>& verticesB,
    const glm::mat4& transformB)
{
    auto supportA = [&verticesA, &transformA](const glm::vec3& dir) {
        return SupportPolyhedron(verticesA, transformA, dir);
    };
    
    auto supportB = [&verticesB, &transformB](const glm::vec3& dir) {
        return SupportPolyhedron(verticesB, transformB, dir);
    };
    
    return GJK_Intersect(supportA, supportB);
}

GJKResult GJK_SphereVsPolyhedron(
    const glm::vec3& sphereCenter,
    float sphereRadius,
    const std::vector<glm::vec3>& polyVertices,
    const glm::mat4& polyTransform)
{
    auto supportA = [&sphereCenter, &sphereRadius](const glm::vec3& dir) {
        return SupportSphere(sphereCenter, sphereRadius, dir);
    };
    
    auto supportB = [&polyVertices, &polyTransform](const glm::vec3& dir) {
        return SupportPolyhedron(polyVertices, polyTransform, dir);
    };
    
    return GJK_Intersect(supportA, supportB);
}

bool GJK_BoxVsSphere(
    const glm::vec3& boxCenter,
    const glm::vec3& boxHalfExtents,
    const glm::mat4& boxRotation,
    const glm::vec3& sphereCenter,
    float sphereRadius)
{
    // Transform sphere center to box local space
    glm::mat3 rotMat = glm::mat3(boxRotation);
    glm::vec3 localSphereCenter = glm::transpose(rotMat) * (sphereCenter - boxCenter);
    
    // Find closest point on box to sphere center
    glm::vec3 closestPoint(
        glm::clamp(localSphereCenter.x, -boxHalfExtents.x, boxHalfExtents.x),
        glm::clamp(localSphereCenter.y, -boxHalfExtents.y, boxHalfExtents.y),
        glm::clamp(localSphereCenter.z, -boxHalfExtents.z, boxHalfExtents.z)
    );
    
    // Check distance
    glm::vec3 diff = localSphereCenter - closestPoint;
    float distSq = glm::dot(diff, diff);
    
    return distSq <= sphereRadius * sphereRadius;
}

// ============================================================================
// SWEEP AND PRUNE
// ============================================================================

void SweepAndPrune::AddBody(std::shared_ptr<RigidBody> body) {
    BodyBounds bounds;
    bounds.body = body;
    // Initialize bounds from body's AABB
    bodies.push_back(bounds);
}

void SweepAndPrune::RemoveBody(std::shared_ptr<RigidBody> body) {
    bodies.erase(
        std::remove_if(bodies.begin(), bodies.end(),
            [&body](const BodyBounds& b) { return b.body == body; }),
        bodies.end()
    );
}

void SweepAndPrune::Update() {
    // Update AABBs from bodies and maintain sorted order using insertion sort
    // This exploits temporal coherence - objects don't move far between frames
    
    for (int axis = 0; axis < 3; axis++) {
        // Update bounds from body positions
        for (size_t i = 0; i < bodies.size(); i++) {
            auto& body = bodies[i].body;
            glm::vec3 halfExtents = body->scale * 0.5f;
            
            // Compute AABB in world space
            bodies[i].min[axis] = body->position[axis] - halfExtents[axis];
            bodies[i].max[axis] = body->position[axis] + halfExtents[axis];
        }
        
        // Initialize sorted indices if empty
        if (sorted[axis].empty()) {
            sorted[axis].resize(bodies.size());
            for (size_t i = 0; i < bodies.size(); i++) {
                sorted[axis][i] = static_cast<int>(i);
            }
        }
        
        // Use insertion sort to maintain sorted order (exploits coherence)
        // Insertion sort is O(n) for nearly sorted data
        for (size_t i = 1; i < bodies.size(); i++) {
            int key = sorted[axis][i];
            float keyMin = bodies[key].min[axis];
            
            int j = static_cast<int>(i) - 1;
            
            // Move elements that are greater than key to one position ahead
            while (j >= 0 && bodies[sorted[axis][j]].min[axis] > keyMin) {
                sorted[axis][j + 1] = sorted[axis][j];
                j--;
            }
            sorted[axis][j + 1] = key;
        }
    }
}

std::vector<std::pair<std::shared_ptr<RigidBody>, std::shared_ptr<RigidBody>>>
SweepAndPrune::GetPotentialCollisions() {
    std::vector<std::pair<std::shared_ptr<RigidBody>, std::shared_ptr<RigidBody>>> pairs;
    
    if (bodies.empty()) return pairs;
    
    // Use sweep and prune on the first axis (typically X)
    // This is O(n + k) where k is the number of overlapping pairs
    const int axis = 0;  // Primary axis for broadphase
    
    // Active set of bodies that overlap on current axis
    std::vector<int> activeSet;
    activeSet.reserve(bodies.size());
    
    // Sweep through sorted bodies
    for (size_t i = 0; i < bodies.size(); i++) {
        int currentIdx = sorted[axis][i];
        auto& current = bodies[currentIdx];
        
        // Remove bodies from active set that no longer overlap
        activeSet.erase(
            std::remove_if(activeSet.begin(), activeSet.end(),
                [this, &current, axis](int idx) {
                    return bodies[idx].max[axis] < current.min[axis];
                }),
            activeSet.end()
        );
        
        // All bodies in active set overlap with current body on this axis
        // Add pairs (will be filtered by other axes)
        for (int otherIdx : activeSet) {
            // Check full 3D AABB overlap
            bool overlap = true;
            for (int a = 1; a < 3; a++) {  // Check Y and Z axes
                if (current.max[a] < bodies[otherIdx].min[a] ||
                    current.min[a] > bodies[otherIdx].max[a]) {
                    overlap = false;
                    break;
                }
            }
            
            if (overlap) {
                pairs.push_back({current.body, bodies[otherIdx].body});
            }
        }
        
        // Add current body to active set
        activeSet.push_back(currentIdx);
    }
    
    return pairs;
}

// ============================================================================
// SPATIAL HASH GRID
// ============================================================================

SpatialHashGrid::SpatialHashGrid(float cellSize) : cellSize(cellSize) {}

void SpatialHashGrid::Clear() {
    grid.clear();
}

int SpatialHashGrid::Hash(int x, int y, int z) const {
    // Simple hash function
    const int p1 = 73856093;
    const int p2 = 19349663;
    const int p3 = 83492791;
    return (x * p1) ^ (y * p2) ^ (z * p3);
}

void SpatialHashGrid::AddObject(std::shared_ptr<RigidBody> body) {
    // Get body's AABB
    glm::vec3 minBounds = body->position - glm::vec3(1.0f);
    glm::vec3 maxBounds = body->position + glm::vec3(1.0f);
    
    // Hash all cells the body occupies
    int minX = static_cast<int>(std::floor(minBounds.x / cellSize));
    int maxX = static_cast<int>(std::floor(maxBounds.x / cellSize));
    int minY = static_cast<int>(std::floor(minBounds.y / cellSize));
    int maxY = static_cast<int>(std::floor(maxBounds.y / cellSize));
    int minZ = static_cast<int>(std::floor(minBounds.z / cellSize));
    int maxZ = static_cast<int>(std::floor(maxBounds.z / cellSize));
    
    for (int x = minX; x <= maxX; x++) {
        for (int y = minY; y <= maxY; y++) {
            for (int z = minZ; z <= maxZ; z++) {
                int h = Hash(x, y, z);
                grid[h].push_back(body);
            }
        }
    }
}

std::vector<std::shared_ptr<RigidBody>> SpatialHashGrid::GetNeighbors(
    std::shared_ptr<RigidBody> body)
{
    std::vector<std::shared_ptr<RigidBody>> neighbors;
    
    // Get cells body occupies
    glm::vec3 pos = body->position;
    int x = static_cast<int>(std::floor(pos.x / cellSize));
    int y = static_cast<int>(std::floor(pos.y / cellSize));
    int z = static_cast<int>(std::floor(pos.z / cellSize));
    
    // Check all 27 neighboring cells
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dz = -1; dz <= 1; dz++) {
                int h = Hash(x + dx, y + dy, z + dz);
                auto it = grid.find(h);
                if (it != grid.end()) {
                    for (const auto& other : it->second) {
                        if (other != body) {
                            neighbors.push_back(other);
                        }
                    }
                }
            }
        }
    }
    
    return neighbors;
}

// ============================================================================
// PHYSICS ISLAND (SLEEPING)
// ============================================================================

void PhysicsIsland::UpdateSleep() {
    if (isSleeping) return;
    
    // Check if all bodies are nearly stationary
    float maxVelSq = 0.0f;
    for (const auto& body : bodies) {
        float velSq = glm::dot(body->velocity, body->velocity);
        maxVelSq = std::max(maxVelSq, velSq);
    }
    
    if (maxVelSq < SLEEP_THRESHOLD * SLEEP_THRESHOLD) {
        sleepTimer += MAX_SUBSTEP_DT;
        if (sleepTimer >= SLEEP_TIME) {
            isSleeping = true;
        }
    } else {
        sleepTimer = 0.0f;
    }
}

bool PhysicsIsland::CanSleep() const {
    return isSleeping;
}

void PhysicsIsland::WakeUp() {
    isSleeping = false;
    sleepTimer = 0.0f;
}
