#pragma once
// =============================================================================
// VelocityConstraints.h — Velocity-Level Constraint Framework via Jacobians
// =============================================================================
// Replaces the legacy *position-based* constraints in Constraint.cpp
// (JointConstraint/DistanceConstraint/SpringConstraint), which "manually push
// the position vector" and never touch angular velocity. This module
// implements the Velocity-Level Constraint Framework via Jacobians described in
// the `todo` file, using a Sequential Impulse (PGS) solver with warm starting
// and split-impulse position correction.
//
// Design (physics proposal in `todo`):
//  - Bodies are referenced by index into a contiguous `bodies` array — no
//    shared_ptr churn per step (matches PhysicsWorld's BodyHandle style).
//  - Constraints are stored as flat, cache-line-aligned structs (the
//    CompressedDistanceConstraint store is SIMD-friendly for a future
//    AVX2/AVX-512 block solver).
//  - Each constraint is a set of Jacobian rows (a Plane = 1 row, a distance
//    joint = 1 row, a 5-DOF Hinge = 5 rows). The generic solver iterates rows;
//    a 5-DOF joint *block* mass matrix is the natural follow-on and reuses
//    these row primitives.
//
// Splitter: velocity and position are solved separately. The velocity pass
// enforces the *one-sided* condition (a body may not accelerate into a plane)
// and preserves angular velocity (JwA = 0). The split-impulse position pass
// removes residual penetration WITHOUT adding kinetic energy — exactly the
// behaviour §1 asks for, and the opposite of the old code which both ignored
// rotation and injected fake kinetic energy via positional snaps.
//
// Header-only; depends only on RigidBody.h. Bodies are mutated directly:
//   - velocity / angularVelocity    (velocity solve)
//  - position / tempPosition        (split-impulse position correction)
// =============================================================================
#include "RigidBody.h"

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>
#include <cstddef>     // offsetof
#include <algorithm>
#include <cmath>

namespace vel {

constexpr int kStaticBody = -1;   // immovable ground/world

// ---------------------------------------------------------------------------
// §motor: velocity motor — a single Jacobian row whose `bias` is a target
// velocity, clamped to [−maxForce, +maxForce]. Reusing JacobianRow means the
// existing sequential-impulse pipeline solves it for free (no new solver).
// ---------------------------------------------------------------------------
struct HingeBlockMotorParams {
    int    bodyA = kStaticBody;
    int    bodyB = kStaticBody;
    glm::vec3 localAxisA{0.0f, 1.0f, 0.0f};

    float  targetVelocity = 0.0f; // rad/s (angular) or m/s (linear)
    float  maxForce = 500.0f;      // torque / force cap
    bool   isAngular = true;      // rotational motor along the axis, else linear
};

// -----------------------------------------------------------------------------
// Contiguous index-based data layout (physics proposal §4).
// Stored verbatim so a future SIMD distance/block solver can stream 4-8 of
// these from a flat, aligned array with no pointer chasing. The velocity
// solver keeps one CompressedDistanceConstraint per addDistanceConstraint()
// alongside the Jacobian rows that drive its velocity/position solve.
// -----------------------------------------------------------------------------
struct alignas(16) CompressedDistanceConstraint {
    int32_t   bodyA = kStaticBody;
    int32_t   bodyB = kStaticBody;
    float     restLength = 1.0f;
    float     stiffness = 1.0f;
    float     lambdaAccum = 0.0f;     // warm-start impulse from the previous frame
    float     _pad0 = 0.0f;
    glm::vec3 anchorA{0.0f};         // world-space attachment point on A
    glm::vec3 anchorB{0.0f};         // world-space attachment point on B
};
static_assert(sizeof(CompressedDistanceConstraint) == 48,
              "CompressedDistanceConstraint must be 48B (3 x vec4 / cache line)");

// One Jacobian row: maps the 12-DOF generalized velocity
//   v = [vA; wA; vB; wB]  (3+3+3+3)  to a scalar  v_c = J . v.
//
// Plane (§1) collapses to J = [n^T 0^T 0^T 0^T]: a 1D *linear* constraint with
// zero angular Jacobian, so angular velocity is preserved — the opposite of the
// legacy code which ignored rotation entirely.
struct JacobianRow {
    int       bodyA = kStaticBody;
    int       bodyB = kStaticBody;
    bool      isPlane = false;       // 1-sided plane contact (lambda >= 0)
    bool      isHinge = false;       // row belongs to a 5-DOF hinge BLOCK joint
    bool      isMotor = false;       // velocity motor: bias = target velocity
    bool      _pad = false;
    glm::vec3 JvA{0.0f};             // linear Jacobian for A
    glm::vec3 JwA{0.0f};             // angular Jacobian for A
    glm::vec3 JvB{0.0f};             // linear Jacobian for B (signs = 3rd law)
    glm::vec3 JwB{0.0f};             // angular Jacobian for B
    float     bias = 0.0f;           // velocity-space bias (restitution/drift)
    float     massNormal = 0.0f;     // 1 / (J M^-1 J^T)
    float     impulse = 0.0f;        // accumulated (warm-started) impulse
    float     positionImpulse = 0.0f;// split-impulse accumulator
    float     minImpulse = -FLT_MAX; // clamp lower (0 for 1-sided plane)
    float     maxImpulse =  FLT_MAX; // clamp upper
    float     penetration = 0.0f;    // live constraint-space error C
    float     restitution = 0.0f;    // bounce coefficient
    float     baumgarteBeta = 0.0f; // drift correction strength
    float     planeD = 0.0f;         // plane: C = -(n.x + d); d folded to planeD
    float     restLength = 0.0f;     // distance: C = |d| - restLength
    float     invMassA = 0.0f;       // cached 1/m for warm start
    float     invMassB = 0.0f;
};

// -----------------------------------------------------------------------------
// Velocity-level constraint solver.
// -----------------------------------------------------------------------------
class ConstraintSolver {
public:
    explicit ConstraintSolver(std::vector<RigidBody>& bodies) : m_bodies(bodies) {}

    void reset(bool warmStart) {
        m_rows.clear();
        m_distanceConstraints.clear();
        m_warmStart = warmStart;
    }

    // --- §1 PlaneConstraint ------------------------------------------------
    // 1D linear Jacobian keeping a body on the +n side of n.x + d = 0
    // (n points toward the body side). One-sided: lambda >= 0; angular velocity
    // untouched (JwA = 0). `restitution` bounces on impact.
    void addPlaneConstraint(int bodyIndex, const glm::vec3& planeNormal,
                            float planeDistance, float restitution = 0.0f,
                            float baumgarteBeta = 0.2f);

    // --- Distance / ball-and-swing joint ----------------------------------
    // Keeps two bodies at `restLength` (velocity-level, bilateral; drift
    // corrected by the split-impulse pass).
    struct DistanceParams {
        int    bodyA = kStaticBody;
        int    bodyB = kStaticBody;
        float  restLength = 1.0f;
        float  stiffness = 1.0f;
    };
    void addDistanceConstraint(const DistanceParams& p,
                               const glm::vec3& anchorA, const glm::vec3& anchorB);

    // Sequential impulse: warm start + velocity PGS, then split-impulse
    // position correction (positions fixed, velocities untouched). Handles
    // plane + distance rows (scalar PGS). Motor rows (add5DOFMotor) are
    // solved here too: their `bias` carries the target velocity.
    void solve(float dt, int velocityIterations = 8, int positionIterations = 4);

    // ------------------------------------------------------------------
    // §2 HingeConstraint (5-DOF rigid revolute joint) solved as a *block*:
    // 3 translational (point-to-point) + 2 rotational (axis alignment)
    // Jacobians are coupled via the 5x5 block mass matrix K = J M^-1 J^T and
    // solved simultaneously (Catto "block solver") — NOT one scalar at a time.
    // Recomputes the angular Jacobians each call because they depend on the
    // bodies' current orientation.
    // ------------------------------------------------------------------
    // Local hinge frames are recovered from the current body poses so the
    // pivot point and hinge axis stay correct as the bodies rotate.
    void addHingeConstraint(int bodyA, int bodyB,
                            const glm::vec3& pivot,      // world-space hinge pivot
                            const glm::vec3& hingeAxis,  // world-space hinge axis
                            float stiffness = 1.0f);

    // Block solve for hinge joints only (the opt-in velocity-constraint pass
    // wired into PhysicsWorld::step). Plane/distance rows are left for solve().
    void solveHinges(float dt, int velocityIterations = 8, int positionIterations = 4);

    size_t rowCount() const { return m_rows.size(); }
    size_t distanceCount() const { return m_distanceConstraints.size(); }
    size_t hingeCount() const { return m_hingeSpecs.size(); }

    // ------------------------------------------------------------------
    // §motor: velocity motor (angular or linear) driving a single DOF toward
    // `targetVelocity`, clamped to [−maxForce, +maxForce]. Solved by solve()
    // together with planes/distance — its `bias` carries the target velocity,
    // so it does not need the block path.
    // ------------------------------------------------------------------
    void add5DOFMotor(const HingeBlockMotorParams& p);

private:
    std::vector<RigidBody>& m_bodies;
    std::vector<JacobianRow> m_rows;
    std::vector<CompressedDistanceConstraint> m_distanceConstraints;
    bool m_warmStart = false;

    // One spec per hinge: the 5 Jacobian rows are stored contiguously in
    // m_rows starting at `rowStart`; the spec caches the body-local hinge
    // frames so the angular Jacobians can be rebuilt every solve.
    struct HingeSpec {
        int   bodyA = kStaticBody;
        int   bodyB = kStaticBody;
        int   rowStart = -1;             // index into m_rows of the first of 5 rows
        float stiffness = 1.0f;
        glm::vec3 anchorA_local{0.0f};   // pivot in A's local frame
        glm::vec3 anchorB_local{0.0f};   // pivot in B's local frame
        glm::vec3 axisA_local{0.0f};     // hinge axis in A's local frame (unit)
        glm::vec3 axisB_local{0.0f};     // hinge axis in B's local frame (unit)
        glm::vec3 s1_local{0.0f};        // reference ⊥ axisA, fixed to A (unit)
        glm::vec3 s2_local{0.0f};        // reference ⊥ axisA & ⊥ s1 (unit)
    };
    std::vector<HingeSpec> m_hingeSpecs;

    // Bodies referenced by `kStaticBody` (-1) OR with isStatic==true are
    // immovable: zero inverse mass / inertia so the solver never injects
    // impulses into the world or static scenery.
    float invMass(int idx) const {
        return (idx == kStaticBody || m_bodies[idx].isStatic) ? 0.0f : m_bodies[idx].invMass();
    }
    const glm::vec3& linVel(int idx) const {
        static const glm::vec3 zero(0.0f);
        return (idx == kStaticBody || m_bodies[idx].isStatic) ? zero : m_bodies[idx].velocity;
    }
    const glm::vec3& angVel(int idx) const {
        static const glm::vec3 zero(0.0f);
        return (idx == kStaticBody || m_bodies[idx].isStatic) ? zero : m_bodies[idx].angularVelocity;
    }
    glm::mat3 invInertia(int idx) const {
        if (idx == kStaticBody || m_bodies[idx].isStatic) return glm::mat3(0.0f);
        return m_bodies[idx].getInverseInertiaTensor();
    }
};

// ---------------------------------------------------------------------------
// Plane constraint: J = [n^T 0^T 0^T 0^T], one-sided lambda >= 0, JwA = 0
// (angular velocity preserved). Penetration d lives in planeD so C = -(n.x + d).
// ---------------------------------------------------------------------------
inline void ConstraintSolver::addPlaneConstraint(int bodyIndex,
                                                 const glm::vec3& planeNormal,
                                                 float planeDistance,
                                                 float restitution,
                                                 float baumgarteBeta) {
    const glm::vec3 n = glm::normalize(planeNormal);
    JacobianRow r;
    r.bodyA       = bodyIndex;
    r.bodyB       = kStaticBody;
    r.isPlane     = true;
    r.JvA         = n;          // C = n . x + d,  v_rel = n . v
    r.JwA         = glm::vec3(0.0f);
    r.invMassA    = invMass(bodyIndex);
    r.invMassB    = 0.0f;
    r.minImpulse  = 0.0f;       // one-sided: never pull into the plane
    r.maxImpulse  = FLT_MAX;
    r.restitution = restitution;
    r.baumgarteBeta = baumgarteBeta;
    r.planeD      = planeDistance;  // so C = -(dot(n,x) + planeDistance)
    m_rows.push_back(r);
}

// ---------------------------------------------------------------------------
// Distance constraint (ball-and-swing): C = |pb - pa| - restLength.
// JvA = -n, JvB = +n, JwA = -(rA x n), JwB = +(rB x n),  n = (pb-pa)/|pb-pa|.
// Stored in the contiguous CompressedDistanceConstraint array too.
// ---------------------------------------------------------------------------
inline void ConstraintSolver::addDistanceConstraint(const DistanceParams& p,
                                                    const glm::vec3& anchorA,
                                                    const glm::vec3& anchorB) {
    CompressedDistanceConstraint dc;
    dc.bodyA = p.bodyA;
    dc.bodyB = p.bodyB;
    dc.restLength = p.restLength;
    dc.stiffness = p.stiffness;
    dc.lambdaAccum = m_warmStart ? dc.lambdaAccum : 0.0f;
    dc.anchorA = anchorA;
    dc.anchorB = anchorB;
    m_distanceConstraints.push_back(dc);

    const int a = p.bodyA, b = p.bodyB;
    glm::vec3 pa = (a != kStaticBody) ? m_bodies[a].position : glm::vec3(0.0f);
    glm::vec3 pb = (b != kStaticBody) ? m_bodies[b].position : glm::vec3(0.0f);
    glm::vec3 d = pb - pa;
    float dist = glm::length(d);
    if (dist < 1e-6f) { d = glm::vec3(0.0f, 1e-5f, 0.0f); dist = 1e-5f; }
    const glm::vec3 n = d / dist;
    glm::vec3 rA = (a != kStaticBody) ? (anchorA - m_bodies[a].position) : glm::vec3(0.0f);
    glm::vec3 rB = (b != kStaticBody) ? (anchorB - m_bodies[b].position) : glm::vec3(0.0f);

    JacobianRow r;
    r.bodyA      = a;  r.bodyB = b;
    r.isPlane    = false;
    r.JvA        = -n;
    r.JvB         =  n;
    r.JwA        = -glm::cross(rA, n);
    r.JwB         =  glm::cross(rB, n);
    r.invMassA   = invMass(a);
    r.invMassB   = invMass(b);
    r.minImpulse = -FLT_MAX;
    r.maxImpulse = FLT_MAX;
    r.restLength = p.restLength;
    r.penetration = dist - p.restLength;
    m_rows.push_back(r);
}

// ---------------------------------------------------------------------------
// Sequential impulse: warm start + velocity PGS + split-impulse position fix.
// ---------------------------------------------------------------------------
inline void ConstraintSolver::solve(float dt, int velocityIterations, int positionIterations) {
    // `dt` is reserved for dt-scaled velocity bias (restitution impulse uses an
    // instantaneous model; the position pass uses a fixed positional beta).
    (void)dt;

    // (1) Live penetration + velocity-space bias.
    for (JacobianRow& row : m_rows) {
        if (row.isPlane) {
            const glm::vec3 x = m_bodies[row.bodyA].position;
            row.penetration = -(glm::dot(row.JvA, x) + row.planeD);   // inside => >0
            const float vRel = glm::dot(row.JvA, linVel(row.bodyA));
            // Velocity enforces "no approach into the plane"; restitution adds
            // bounce only while approaching. Penetration drift is fixed by the
            // (zero-energy) position pass, NOT here — so a resting body doesn't
            // jitter or gain energy.
            row.bias = -row.restitution * std::min(vRel, 0.0f);
        } else if (row.isMotor) {
            // Velocity motor: `bias` (the target velocity) is set at
            // registration and must survive the precompute; no positional fix.
            row.penetration = 0.0f;
        } else {
            const glm::vec3 pa = (row.bodyA != kStaticBody) ? m_bodies[row.bodyA].position : glm::vec3(0,0,0);
            const glm::vec3 pb = (row.bodyB != kStaticBody) ? m_bodies[row.bodyB].position : glm::vec3(0,0,0);
            const float dist = glm::length(pb - pa);
            row.penetration = dist - row.restLength;   // signed error
            // Bilateral distance: velocity drives relative speed toward 0.
            row.bias = 0.0f;
        }
    }

    // (2) Warm start — replay each row's accumulated impulse.
    for (const JacobianRow& row : m_rows) {
        if (row.bodyA != kStaticBody && row.invMassA > 0.0f) {
            m_bodies[row.bodyA].velocity        += row.JvA * row.impulse * row.invMassA;
            m_bodies[row.bodyA].angularVelocity += invInertia(row.bodyA) * row.JwA * row.impulse;
        }
        if (row.bodyB != kStaticBody && row.invMassB > 0.0f) {
            m_bodies[row.bodyB].velocity        += row.JvB * row.impulse * row.invMassB;
            m_bodies[row.bodyB].angularVelocity += invInertia(row.bodyB) * row.JwB * row.impulse;
        }
    }

    // (3) Velocity PGS — Projected Gauss-Seidel with accumulation.
    for (int it = 0; it < velocityIterations; ++it) {
        for (JacobianRow& row : m_rows) {
            const int a = row.bodyA, b = row.bodyB;
            const glm::vec3& va = linVel(a);
            const glm::vec3& wa = angVel(a);
            const glm::vec3& vb = linVel(b);
            const glm::vec3& wb = angVel(b);
            const float invA = row.invMassA, invB = row.invMassB;

            // Effective mass K = J M^-1 J^T.
            float K = 0.0f;
            K += glm::dot(row.JvA, row.JvA) * invA;
            if (a != kStaticBody) K += glm::dot(row.JwA, invInertia(a) * row.JwA);
            K += glm::dot(row.JvB, row.JvB) * invB;
            if (b != kStaticBody) K += glm::dot(row.JwB, invInertia(b) * row.JwB);
            const float massN = (K > 1e-12f) ? (1.0f / K) : 0.0f;
            row.massNormal = massN;

            // Constraint-space relative velocity.
            float vRel = 0.0f;
            vRel += glm::dot(row.JvA, va);
            vRel += glm::dot(row.JwA, wa);
            vRel += glm::dot(row.JvB, vb);
            vRel += glm::dot(row.JwB, wb);

            // lambda_new = clamp(lambda + massN * (bias - vRel), min, max).
            float lambda = row.impulse + massN * (row.bias - vRel);
            lambda = std::clamp(lambda, row.minImpulse, row.maxImpulse);
            const float dLambda = lambda - row.impulse;
            row.impulse = lambda;

            if (a != kStaticBody && invA > 0.0f) {
                m_bodies[a].velocity        += row.JvA * dLambda * invA;
                m_bodies[a].angularVelocity += invInertia(a) * row.JwA * dLambda;
            }
            if (b != kStaticBody && invB > 0.0f) {
                m_bodies[b].velocity        += row.JvB * dLambda * invB;
                m_bodies[b].angularVelocity += invInertia(b) * row.JwB * dLambda;
            }
        }
    }

    // (4) Split-impulse position correction — velocities left untouched.
    //     Direct, mass-weighted projection toward the constraint target.
    const float posBeta = 0.2f;   // positional Baumgarte factor
    const float slop = 0.001f;    // penetration tolerance
    for (int it = 0; it < positionIterations; ++it) {
        for (JacobianRow& row : m_rows) {
            if (row.isMotor) continue;       // velocity motor: no position target
            if (row.isPlane) {
                const glm::vec3 x = m_bodies[row.bodyA].position;
                // C = penetration depth (>=0 when the body is inside the plane).
                float C = -(glm::dot(row.JvA, x) + row.planeD);
                C = std::max(C - slop, 0.0f);
                if (C == 0.0f) continue;
                if (row.invMassA > 0.0f) {
                    // Single body vs immovable ground: full correction along n.
                    m_bodies[row.bodyA].position += row.JvA * (posBeta * C);
                    m_bodies[row.bodyA].tempPosition = m_bodies[row.bodyA].position;
                }
            } else {
                const glm::vec3 pa = (row.bodyA != kStaticBody) ? m_bodies[row.bodyA].position : glm::vec3(0.0f);
                const glm::vec3 pb = (row.bodyB != kStaticBody) ? m_bodies[row.bodyB].position : glm::vec3(0.0f);
                glm::vec3 d = pb - pa;
                const float dist = glm::length(d);
                if (dist < 1e-6f) continue;
                const glm::vec3 n = d / dist;
                // C = signed distance error (C>0 => stretched, C<0 => compressed).
                const float C = dist - row.restLength;
                const float invSum = row.invMassA + row.invMassB;
                if (invSum <= 0.0f) continue;
                // Corrective displacement magnitude, split by inverse-mass weighting.
                const float corr = posBeta * C / invSum;
                if (row.bodyA != kStaticBody) {
                    m_bodies[row.bodyA].position += n * (corr * row.invMassA);
                    m_bodies[row.bodyA].tempPosition = m_bodies[row.bodyA].position;
                }
                if (row.bodyB != kStaticBody) {
                    m_bodies[row.bodyB].position += n * (-corr * row.invMassB);
                    m_bodies[row.bodyB].tempPosition = m_bodies[row.bodyB].position;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 5×5 dense linear solve  A·x = b  via Gauss-Jordan with partial pivoting.
// Returns false (singular) if A is degenerate. Used by the hinge block solver.
// ---------------------------------------------------------------------------
inline bool solveLinear5x5(const float A[5][5], const float b[5], float x[5]) {
    float M[5][6];
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 5; ++j) M[i][j] = A[i][j];
        M[i][5] = b[i];
        x[i] = 0.0f;
    }
    for (int col = 0; col < 5; ++col) {
        int piv = col;
        float maxVal = std::fabs(M[col][col]);
        for (int r = col + 1; r < 5; ++r) {
            float v = std::fabs(M[r][col]);
            if (v > maxVal) { maxVal = v; piv = r; }
        }
        if (maxVal < 1e-12f) return false;     // singular — leave x = 0
        if (piv != col) {
            for (int j = 0; j <= 5; ++j) {
                float tmp = M[col][j];
                M[col][j] = M[piv][j];
                M[piv][j] = tmp;
            }
        }
        float pv = M[col][col];
        for (int j = 0; j <= 5; ++j) M[col][j] /= pv;
        for (int r = 0; r < 5; ++r) {
            if (r == col) continue;
            float f = M[r][col];
            if (f == 0.0f) continue;
            for (int j = 0; j <= 5; ++j) M[r][j] -= f * M[col][j];
        }
    }
    for (int i = 0; i < 5; ++i) x[i] = M[i][5];
    return true;
}

// ---------------------------------------------------------------------------
// HingeConstraint: 5-DOF rigid revolute joint.
//   - rows 0..2 : point-to-point (ball) keeping the two anchor points — the
//     world pivot — coincident. J = [ -I  -rA×  +I  +rB× ].
//   - rows 3..4 : two rotational rows aligning body B's hinge axis with the
//     reference axes s1,s2 fixed to body A, removing the 2 swing DOFs and
//     leaving rotation about the hinge axis free.
// Local frames are recovered from the bodies' current pose (so the hinge
// survives arbitrary rotation); `stiffness` scales the position fix.
// ---------------------------------------------------------------------------
inline void ConstraintSolver::addHingeConstraint(int bodyA, int bodyB,
                                                 const glm::vec3& pivotWorld,
                                                 const glm::vec3& hingeAxisWorld,
                                                 float stiffness) {
    const glm::vec3 axis = glm::normalize(hingeAxisWorld);
    const glm::vec3 n  = (glm::dot(axis, axis) < 1e-8f) ? glm::vec3(0, 0, 1.0f)
                                                         : axis;

    HingeSpec spec;
    spec.bodyA = bodyA;   spec.bodyB = bodyB;
    spec.stiffness = stiffness;
    spec.rowStart = static_cast<int>(m_rows.size());

    // Body-local pivot = R^-1 (pivot - pos). Body-local axis = R^-1 axis.
    if (bodyA != kStaticBody) {
        const glm::mat3 RA = glm::mat3_cast(m_bodies[bodyA].rotation);
        spec.anchorA_local = glm::transpose(RA) * (pivotWorld - m_bodies[bodyA].position);
        spec.axisA_local   = glm::transpose(RA) * n;
    }
    if (bodyB != kStaticBody) {
        const glm::mat3 RB = glm::mat3_cast(m_bodies[bodyB].rotation);
        spec.anchorB_local = glm::transpose(RB) * (pivotWorld - m_bodies[bodyB].position);
        spec.axisB_local   = glm::transpose(RB) * n;
    }

    // Two reference axes fixed in A, perpendicular to the hinge axis, spanning
    // the swing plane. Recovered from the body-local axis at registration; they
    // stay locked to A's frame and therefore rotate with body A.
    glm::vec3 s1 = (std::fabs(n.x) < 0.7f) ? glm::vec3(1, 0, 0)
                                             : glm::vec3(0, 1, 0);
    s1 = glm::normalize(s1 - n * glm::dot(s1, n));
    const glm::vec3 s2 = glm::cross(n, s1);
    const glm::mat3 RA0 = glm::mat3_cast(m_bodies[bodyA].rotation);
    const glm::mat3 RAi = glm::transpose(RA0);
    spec.s1_local = RAi * s1;
    spec.s2_local = RAi * s2;
    m_hingeSpecs.push_back(spec);

    // 3 translational rows (point-to-point along world x/y/z).
    const float invA = invMass(bodyA), invB = invMass(bodyB);
    for (int k = 0; k < 3; ++k) {
        JacobianRow r;
        r.bodyA = bodyA;  r.bodyB = bodyB;
        r.isHinge = true;
        const glm::vec3 e(k == 0 ? 1.0f : 0.0f,
                          k == 1 ? 1.0f : 0.0f,
                          k == 2 ? 1.0f : 0.0f);
        r.JvA = -e;  r.JvB = e;           // linear part (rebuilt angular below)
        r.invMassA = invA;  r.invMassB = invB;
        r.minImpulse = -FLT_MAX;
        r.maxImpulse = FLT_MAX;
        r.restLength = 0.0f;             // anchors must coincide (pivot)
        m_rows.push_back(r);
    }
    // 2 rotational rows (axis alignment). Angular Jacobians are rebuilt each
    // solve; leave Jv = 0 here, filled in solveHinges().
    for (int k = 0; k < 2; ++k) {
        JacobianRow r;
        r.bodyA = bodyA;  r.bodyB = bodyB;
        r.isHinge = true;
        r.invMassA = invA;  r.invMassB = invB;
        r.minImpulse = -FLT_MAX;
        r.maxImpulse = FLT_MAX;
        r.restLength = 0.0f;
        m_rows.push_back(r);
    }
}

// ---------------------------------------------------------------------------
// §motor: velocity motor (Part 1 of the `todo` follow-up). A single Jacobian
// row — angular about localAxisA (world-aligned), or linear along it. The row's
// `bias` IS the target velocity; [minImpulse,maxImpulse] caps the torque/force.
// Sitting in m_rows, it is solved by solve() alongside planes/distance.
// ---------------------------------------------------------------------------
inline void ConstraintSolver::add5DOFMotor(const HingeBlockMotorParams& p) {
    const int a = p.bodyA, b = p.bodyB;
    const glm::quat rotA = (a != kStaticBody) ? m_bodies[a].rotation
                                              : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    const glm::vec3 worldAxisA = glm::normalize(rotA * p.localAxisA);

    JacobianRow r;
    r.bodyA = a;  r.bodyB = b;
    r.isPlane = false;
    r.isMotor = true;

    if (p.isAngular) {
        r.JvA = glm::vec3(0.0f);
        r.JvB = glm::vec3(0.0f);
        r.JwA = -worldAxisA;
        r.JwB =  worldAxisA;
    } else {
        r.JvA = -worldAxisA;
        r.JvB =  worldAxisA;
        r.JwA = glm::vec3(0.0f);
        r.JwB = glm::vec3(0.0f);
    }

    r.invMassA = invMass(a);
    r.invMassB = invMass(b);

    r.minImpulse  = -p.maxForce;
    r.maxImpulse  =  p.maxForce;

    r.bias        = p.targetVelocity;  // target velocity IS the velocity bias
    r.penetration = 0.0f;              // no positional correction for a motor
    r.restLength  = 0.0f;
    m_rows.push_back(r);
}

// ---------------------------------------------------------------------------
// Hinge block solve (warm start + velocity block PGS + split-impulse position).
// Only processes rows flagged isHinge; plane/distance rows are ignored here.
// ---------------------------------------------------------------------------
inline void ConstraintSolver::solveHinges(float dt, int velocityIterations,
                                          int positionIterations) {
    (void)dt;

    // Recompute the live angular Jacobians for every hinge row (they rotate
    // with the bodies) and the point-to-point penetration.
    auto rebuild = [&](const HingeSpec& spec, JacobianRow* rows) {
        const int a = spec.bodyA, b = spec.bodyB;
        const glm::mat3 RA = (a != kStaticBody) ? glm::mat3_cast(m_bodies[a].rotation) : glm::mat3(0.0f);
        const glm::mat3 RB = (b != kStaticBody) ? glm::mat3_cast(m_bodies[b].rotation) : glm::mat3(0.0f);
        const glm::vec3 rA = (a != kStaticBody) ? (RA * spec.anchorA_local) : glm::vec3(0.0f);
        const glm::vec3 rB = (b != kStaticBody) ? (RB * spec.anchorB_local) : glm::vec3(0.0f);
        const glm::vec3 aB = (b != kStaticBody) ? (RB * spec.axisB_local) : glm::vec3(0.0f);
        const glm::vec3 s1 = (a != kStaticBody) ? (RA * spec.s1_local) : glm::vec3(0.0f);
        const glm::vec3 s2 = (a != kStaticBody) ? (RA * spec.s2_local) : glm::vec3(0.0f);

        // Rows 0..2: translational point-to-point.
        for (int k = 0; k < 3; ++k) {
            JacobianRow& row = rows[k];
            const glm::vec3 e(k == 0 ? 1.0f : 0.0f,
                              k == 1 ? 1.0f : 0.0f,
                              k == 2 ? 1.0f : 0.0f);
            row.JvA = -e;  row.JvB = e;
            row.JwA = -glm::cross(rA, e);
            row.JwB =  glm::cross(rB, e);
            // Penetration = separation of the two anchor points (signed, per
            // basis axis) — drives the split-impulse point-to-point fix.
            const glm::vec3 sep = (b != kStaticBody ? (m_bodies[b].position + rB) : glm::vec3(0.0f))
                                - (a != kStaticBody ? (m_bodies[a].position + rA) : glm::vec3(0.0f));
            row.penetration = glm::dot(sep, e);
            row.bias = 0.0f;
        }
        // Rows 3..4: rotational axis alignment.
        const glm::vec3 sref[2] = { s1, s2 };
        for (int k = 0; k < 2; ++k) {
            JacobianRow& row = rows[3 + k];
            row.JvA = glm::vec3(0.0f);  row.JvB = glm::vec3(0.0f);
            row.JwA =  glm::cross(sref[k], aB);   // JwA = s × aB
            row.JwB = -row.JwA;                    // symmetric (JwB = -JwA)
            row.penetration = 0.0f;               // angular drift handled by velocity
            row.bias = 0.0f;
        }
    };

    // (2) Warm start — replay accumulated hinge impulses with rebuilt Jacobians.
    for (const HingeSpec& spec : m_hingeSpecs) {
        JacobianRow* R = &m_rows[spec.rowStart];
        rebuild(spec, R);
        for (int k = 0; k < 5; ++k) {
            JacobianRow& row = R[k];
            if (row.bodyA != kStaticBody && row.invMassA > 0.0f) {
                m_bodies[row.bodyA].velocity        += row.JvA * row.impulse * row.invMassA;
                m_bodies[row.bodyA].angularVelocity += invInertia(row.bodyA) * row.JwA * row.impulse;
            }
            if (row.bodyB != kStaticBody && row.invMassB > 0.0f) {
                m_bodies[row.bodyB].velocity        += row.JvB * row.impulse * row.invMassB;
                m_bodies[row.bodyB].angularVelocity += invInertia(row.bodyB) * row.JwB * row.impulse;
            }
        }
    }

    // (3) Velocity block PGS.
    for (int it = 0; it < velocityIterations; ++it) {
        for (const HingeSpec& spec : m_hingeSpecs) {
            JacobianRow* R = &m_rows[spec.rowStart];
            rebuild(spec, R);

            const int a = spec.bodyA, b = spec.bodyB;
            const float invA = R[0].invMassA, invB = R[0].invMassB;
            const glm::mat3 invIa = invInertia(a);
            const glm::mat3 invIb = invInertia(b);
            const glm::vec3& va = linVel(a);
            const glm::vec3& wa = angVel(a);
            const glm::vec3& vb = linVel(b);
            const glm::vec3& wb = angVel(b);

            float vC[5];
            for (int i = 0; i < 5; ++i) {
                vC[i] = glm::dot(R[i].JvA, va) + glm::dot(R[i].JwA, wa)
                      + glm::dot(R[i].JvB, vb) + glm::dot(R[i].JwB, wb);
            }

            float K[5][5];
            for (int i = 0; i < 5; ++i) {
                for (int j = 0; j < 5; ++j) {
                    float kij = 0.0f;
                    kij += glm::dot(R[i].JvA, R[j].JvA) * invA;
                    kij += glm::dot(R[i].JwA, invIa * R[j].JwA);
                    kij += glm::dot(R[i].JvB, R[j].JvB) * invB;
                    kij += glm::dot(R[i].JwB, invIb * R[j].JwB);
                    K[i][j] = kij;
                }
            }

            float rhs[5], dl[5];
            for (int i = 0; i < 5; ++i) rhs[i] = R[i].bias - vC[i]; // bias = 0
            if (!solveLinear5x5(K, rhs, dl)) continue;              // singular → skip

            // λ_new = clamp(λ_prev + Δλ, min, max); apply only the *clamped*
            // impulse delta Δλ = λ_new − λ_prev via v += M^-1 J^T Δλ.
            float deltaLambda[5];
            for (int i = 0; i < 5; ++i) {
                const float lambdaPrev = R[i].impulse;
                const float lambdaNew  = std::clamp(lambdaPrev + dl[i],
                                                     R[i].minImpulse, R[i].maxImpulse);
                deltaLambda[i] = lambdaNew - lambdaPrev;
                R[i].impulse = lambdaNew;
            }
            if (a != kStaticBody && invA > 0.0f) {
                glm::vec3 dv(0.0f), dw(0.0f);
                for (int i = 0; i < 5; ++i) {
                    dv += R[i].JvA * deltaLambda[i];
                    dw += R[i].JwA * deltaLambda[i];
                }
                m_bodies[a].velocity        += dv * invA;
                m_bodies[a].angularVelocity += invIa * dw;
            }
            if (b != kStaticBody && invB > 0.0f) {
                glm::vec3 dv(0.0f), dw(0.0f);
                for (int i = 0; i < 5; ++i) {
                    dv += R[i].JvB * deltaLambda[i];
                    dw += R[i].JwB * deltaLambda[i];
                }
                m_bodies[b].velocity        += dv * invB;
                m_bodies[b].angularVelocity += invIb * dw;
            }
        }
    }

    // (4) Split-impulse position correction (point-to-point only; angular
    //     alignment is handled at the velocity level above). Velocities
    //     untouched.
    const float posBeta = 0.2f;
    for (int it = 0; it < positionIterations; ++it) {
        for (const HingeSpec& spec : m_hingeSpecs) {
            JacobianRow* R = &m_rows[spec.rowStart];
            rebuild(spec, R);
            const int a = spec.bodyA, b = spec.bodyB;
            const glm::mat3 RA = (a != kStaticBody) ? glm::mat3_cast(m_bodies[a].rotation) : glm::mat3(0.0f);
            const glm::mat3 RB = (b != kStaticBody) ? glm::mat3_cast(m_bodies[b].rotation) : glm::mat3(0.0f);
            const glm::vec3 rA = (a != kStaticBody) ? (RA * spec.anchorA_local) : glm::vec3(0.0f);
            const glm::vec3 rB = (b != kStaticBody) ? (RB * spec.anchorB_local) : glm::vec3(0.0f);
            const glm::vec3 pa = (a != kStaticBody) ? (m_bodies[a].position + rA) : glm::vec3(0.0f);
            const glm::vec3 pb = (b != kStaticBody) ? (m_bodies[b].position + rB) : glm::vec3(0.0f);
            const glm::vec3 sep = pb - pa;            // 0 when pivots coincide
            const float invSum = R[0].invMassA + R[0].invMassB;
            if (invSum <= 0.0f) continue;
            const float corr = posBeta * glm::length(sep) / invSum;
            const glm::vec3 n_sep = (glm::dot(sep, sep) > 1e-12f) ? sep / glm::length(sep) : glm::vec3(0.0f);
            if (a != kStaticBody) {
                m_bodies[a].position += n_sep * ( corr * R[0].invMassA);
                m_bodies[a].tempPosition = m_bodies[a].position;
            }
            if (b != kStaticBody) {
                m_bodies[b].position += n_sep * (-corr * R[0].invMassB);
                m_bodies[b].tempPosition = m_bodies[b].position;
            }
        }
    }
}

} // namespace vel

// ===========================================================================
// Compile-Time Static Constraint Pipeline (todo follow-up Part 2).
// Mirrors StaticConstraintPipeline.h's CRTP design but lives here in the
// clean `vel_static` namespace so it never clashes with the legacy
// Constraint.h types (which share names like DistanceConstraint) that may be
// pulled in by other TUs. Each constraint is fully inlined/unrolled at compile
// time — used for large fixed sets of environmental joints (swing gates, chain
// links, etc.) where the per-row overhead of ConstraintSolver is wasteful.
// ===========================================================================
namespace vel_static {

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

template <typename Derived>
class StaticConstraintBase {
public:
    __attribute__((always_inline)) inline void
    Solve(StaticPhysicsWorld& world, float dt) {
        static_cast<Derived*>(this)->SolveImpl(world, dt);
    }
};

// Super-fast positional projection that locks a single translational DOF:
// motion along `lockAxis` is removed; the two perpendicular axes stay free.
// Used by CompileTimeConstraintPipeline below.
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

        if (!a->isStatic) a->position += correction * (wA / wTotal);
        if (!b->isStatic) b->position -= correction * (wB / wTotal);
    }

private:
    int       idxA, idxB;
    glm::vec3 localLockAxis;
    float     stiffness;
};

// Compile-time tuple unroller.
template <size_t I = 0, typename... Cs>
inline typename std::enable_if<I == sizeof...(Cs), void>::type
UnrollSolve(std::tuple<Cs...>&, StaticPhysicsWorld&, float) {}

template <size_t I = 0, typename... Cs>
inline typename std::enable_if<I < sizeof...(Cs), void>::type
UnrollSolve(std::tuple<Cs...>& t, StaticPhysicsWorld& world, float dt) {
    std::get<I>(t).Solve(world, dt);
    UnrollSolve<I + 1, Cs...>(t, world, dt);
}

template <typename... ConstraintTypes>
class CompileTimeConstraintPipeline {
public:
    explicit CompileTimeConstraintPipeline(ConstraintTypes... cs)
        : constraints(std::make_tuple(std::move(cs)...)) {}

    void Solve(StaticPhysicsWorld& world, float dt, int iterations = 1) {
        for (int step = 0; step < iterations; ++step) {
            UnrollSolve<0, ConstraintTypes...>(constraints, world, dt);
        }
    }

    std::tuple<ConstraintTypes...> constraints;
};

} // namespace vel_static
