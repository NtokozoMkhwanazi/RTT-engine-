#pragma once
// ============================================================================
// StaticConstraintPipeline.h — CRTP Compile-Time Constraint Solver
// ============================================================================
// Replaces virtual-function constraint solving with compile-time template
// resolution. Each constraint type is fully inlined and unrolled at compile
// time, eliminating vtable pointer indirection and enabling the compiler to
// auto-vectorize the inner loop.
//
// Usage:
//   CompileTimeConstraintPipeline<DistanceConstraint, PlaneConstraint>
//       pipeline(distConst, planeConst);
//   pipeline.Solve(world, dt, 8);   // 8 solver iterations, all inlined
// ============================================================================

#include <tuple>
#include <type_traits>
#include <cmath>
#include <glm/glm.hpp>
#include <vector>

// ---- Lightweight rigid body for the static pipeline -------------------------
struct StaticRigidBody {
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    bool      isStatic{false};
    float     mass{1.0f};

    float inverseMass() const {
        return isStatic ? 0.0f : (mass > 0.0f ? 1.0f / mass : 0.0f);
    }
};

struct StaticPhysicsWorld {
    std::vector<StaticRigidBody> bodies;
};

// ---- CRTP base --------------------------------------------------------------
template <typename Derived>
class StaticConstraintBase {
public:
    // Force inline into the caller — no indirect call overhead.
    __attribute__((always_inline)) inline void
    Solve(StaticPhysicsWorld& world, float dt) {
        static_cast<Derived*>(this)->SolveImpl(world, dt);
    }
};

// ---- Distance Constraint (compile-time inlined) -----------------------------
class DistanceConstraint final
    : public StaticConstraintBase<DistanceConstraint> {
public:
    DistanceConstraint(int a, int b, float rest, float stiff = 0.8f)
        : idxA(a), idxB(b), restDist(rest), stiffness(stiff) {}

    __attribute__((always_inline)) inline void
    SolveImpl(StaticPhysicsWorld& world, float) {
        StaticRigidBody* a = &world.bodies[idxA];
        StaticRigidBody* b = &world.bodies[idxB];

        glm::vec3 sep = b->position - a->position;
        float dist = glm::length(sep);
        if (dist < 1e-5f) return;

        float error = dist - restDist;
        glm::vec3 corr = (sep / dist) * error * stiffness;

        float wA = a->inverseMass();
        float wB = b->inverseMass();
        float wTotal = wA + wB;
        if (wTotal < 1e-5f) return;

        if (!a->isStatic) a->position += corr * (wA / wTotal);
        if (!b->isStatic) b->position -= corr * (wB / wTotal);
    }

private:
    int   idxA, idxB;
    float restDist, stiffness;
};

// ---- Plane / Floor Constraint (compile-time inlined) ------------------------
class PlaneConstraint final
    : public StaticConstraintBase<PlaneConstraint> {
public:
    PlaneConstraint(int body, const glm::vec3& normal, float offset)
        : idx(body), planeNormal(glm::normalize(normal)), planeOffset(offset) {}

    __attribute__((always_inline)) inline void
    SolveImpl(StaticPhysicsWorld& world, float) {
        StaticRigidBody* b = &world.bodies[idx];
        if (b->isStatic) return;

        float penetration = glm::dot(b->position, planeNormal) - planeOffset;
        if (penetration < 0.0f) {
            b->position -= penetration * planeNormal;
            float vn = glm::dot(b->velocity, planeNormal);
            if (vn < 0.0f)
                b->velocity -= vn * planeNormal;
        }
    }

private:
    int       idx;
    glm::vec3 planeNormal;
    float     planeOffset;
};

// ---- Hinge Constraint (compile-time inlined) --------------------------------
class HingeConstraint final
    : public StaticConstraintBase<HingeConstraint> {
public:
    HingeConstraint(int a, int b, const glm::vec3& anchor,
                    const glm::vec3& axis, float stiff = 0.3f)
        : idxA(a), idxB(b), anchorPt(anchor),
          hingeAxis(glm::normalize(axis)), stiffness(stiff) {}

    __attribute__((always_inline)) inline void
    SolveImpl(StaticPhysicsWorld& world, float) {
        StaticRigidBody* a = &world.bodies[idxA];
        StaticRigidBody* b = &world.bodies[idxB];

        // Position: bring anchor points together
        glm::vec3 sep = b->position - a->position;
        float dist = glm::length(sep);
        if (dist > 1e-4f) {
            float wA = a->inverseMass();
            float wB = b->inverseMass();
            float wTotal = wA + wB;
            if (wTotal > 1e-5f) {
                glm::vec3 corr = sep * stiffness;
                if (!a->isStatic) a->position += corr * (wA / wTotal);
                if (!b->isStatic) b->position -= corr * (wB / wTotal);
            }
        }
    }

private:
    int       idxA, idxB;
    glm::vec3 anchorPt, hingeAxis;
    float     stiffness;
};

// ---- Compile-Time Inlined 5-DOF Linear Hinge Block --------------------------
// Super-fast positional projection that locks a single translational DOF
// (motion along `lockAxis` is removed); the two perpendicular axes stay free.
// Operates directly on StaticRigidBody positions — no rotational tensors, so
// the compiler can fully inline + vectorize it. Used for thousands of static
// environmental joints (swinging gates, chain-link elements, etc.) via
// CompileTimeConstraintPipeline below.
class Static5DOFHingeBlock final
    : public StaticConstraintBase<Static5DOFHingeBlock> {
public:
    Static5DOFHingeBlock(int a, int b, const glm::vec3& lockAxis, float stiff = 0.8f)
        : idxA(a), idxB(b), localLockAxis(glm::normalize(lockAxis)), stiffness(stiff) {}

    __attribute__((always_inline)) inline void
    SolveImpl(StaticPhysicsWorld& world, float) {
        StaticRigidBody* a = &world.bodies[idxA];
        StaticRigidBody* b = &world.bodies[idxB];

        // Drift of the bodies' separation along the locked axis.
        glm::vec3 separation = b->position - a->position;
        float axialDrift = glm::dot(separation, localLockAxis);
        if (std::fabs(axialDrift) < 1e-5f) return;

        glm::vec3 correction = localLockAxis * (axialDrift * stiffness);

        float wA = a->inverseMass();
        float wB = b->inverseMass();
        float wTotal = wA + wB;
        if (wTotal < 1e-5f) return;

        // Mass-weighted positional update along the locked DOF only.
        if (!a->isStatic) a->position += correction * (wA / wTotal);
        if (!b->isStatic) b->position -= correction * (wB / wTotal);
    }

private:
    int       idxA, idxB;
    glm::vec3 localLockAxis;
    float     stiffness;
};

// ---- Compile-time tuple unroller ---------------------------------------------
// Terminal case: all constraints processed.
template <size_t I = 0, typename... Cs>
inline typename std::enable_if<I == sizeof...(Cs), void>::type
UnrollSolve(std::tuple<Cs...>&, StaticPhysicsWorld&, float) {}

// Recursive case: solve current constraint, advance to next.
template <size_t I = 0, typename... Cs>
inline typename std::enable_if<I < sizeof...(Cs), void>::type
UnrollSolve(std::tuple<Cs...>& t, StaticPhysicsWorld& world, float dt) {
    std::get<I>(t).Solve(world, dt);
    UnrollSolve<I + 1, Cs...>(t, world, dt);
}

// ---- Master compile-time constraint pipeline ---------------------------------
template <typename... ConstraintTypes>
class CompileTimeConstraintPipeline {
public:
    explicit CompileTimeConstraintPipeline(ConstraintTypes... cs)
        : constraints(std::make_tuple(std::move(cs)...)) {}

    // Solve all constraints for `iterations` sub-steps. Every Solve call
    // is fully inlined and the compiler can auto-vectorize the inner loop.
    void Solve(StaticPhysicsWorld& world, float dt, int iterations = 1) {
        for (int step = 0; step < iterations; ++step) {
            UnrollSolve<0, ConstraintTypes...>(constraints, world, dt);
        }
    }

    std::tuple<ConstraintTypes...> constraints;
};
