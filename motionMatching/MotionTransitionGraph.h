#pragma once
#include "MotionDatabase.h"
#include "TransitionClipGenerator.h"
#include "../animationSystem/Animation.h"
#include <map>
#include <memory>
#include <utility>
#include <vector>
#include <set>
#include <string>
#include <functional>

// ============================================================================
// MOTION TRANSITION GRAPH
// ============================================================================
//
// Implements the motion graph structure and SCC-based pruning strategy from
// Kovar & Gleicher "Motion Graphs" (SIGGRAPH 2002), Section 3.4.
//
// The graph is a directed structure where:
//   - Nodes = HybridState values (LOCOMOTION, CROUCH, CROUCH_WALK, COMBAT, ...)
//     stored as opaque ints to avoid circular include with HybridMMFSM.h.
//   - Edges = transition clips (pre-computed smooth blends between databases)
//
// At load time, the graph:
//   1. Builds transition clips between all valid state pairs using
//      TransitionClipGenerator.
//   2. Prunes dead-ends and sinks using Tarjan's SCC algorithm.
//
// At runtime, the graph replaces the instant SetDatabaseExplicit swap:
// instead of instantly swapping database pointers, the MotionMatcher plays
// a pre-baked transition clip. When the clip finishes, the database switch
// completes seamlessly because the transition clip already blended all joints.
// ============================================================================

class MotionTransitionGraph {
public:
    /**
     * A directed edge in the transition graph — a pre-computed transition
     * clip connecting two motion databases.
     */
    struct GraphEdge {
        int from;                   // Source HybridState (opaque int)
        int to;                     // Target HybridState (opaque int)
        std::shared_ptr<Animation> transitionClip;
        float transitionCost;       // Distance metric score (lower = better blend)
        bool valid;                 // False if pruned by SCC analysis
    };

    MotionTransitionGraph();
    ~MotionTransitionGraph() = default;

    /**
     * Build transition clips between all pairs of state databases.
     *
     * For each state pair (from, to), uses TransitionClipGenerator to find
     * the best transition point and generate a smooth blend clip.
     *
     * @param stateDatabases Map of state -> motion database (keys are int)
     * @param skeleton Character skeleton
     * @param transitionFrames Number of frames in each transition clip
     * @param fps Frame rate for transition clips
     */
    void BuildTransitions(
        const std::map<int, MotionDatabase*>& stateDatabases,
        const Skeleton* skeleton,
        int transitionFrames = 40,
        float fps = 120.0f
    );

    /**
     * Prune the graph using Tarjan's strongly-connected-components algorithm.
     *
     * Eliminates edges that lead to dead-ends or sinks — states from which
     * the character cannot generate arbitrarily long streams of motion of the
     * same type (Kovar & Gleicher §3.4).
     *
     * The largest SCC within each movement-type subgraph is retained;
     * all edges not part of it are marked valid=false.
     */
    void PruneGraph();

    /**
     * Get the pre-computed transition clip for a state pair.
     * Returns nullptr if no transition exists or it was pruned.
     */
    std::shared_ptr<Animation> GetTransition(int from, int to) const;

    /**
     * Check if a transition clip exists and is valid (not pruned).
     */
    bool HasTransition(int from, int to) const;

    /**
     * Get the transition cost (distance metric) for a state pair.
     * Returns infinity if no transition exists.
     */
    float GetTransitionCost(int from, int to) const;

    /**
     * Clear all transition clips and edges.
     */
    void Clear() { edges.clear(); nodeSet.clear(); }

    /**
     * Get all outgoing edges from a state.
     */
    std::vector<const GraphEdge*> GetOutgoingEdges(int from) const;

    /**
     * Get all states that are directly reachable from a given state.
     */
    std::vector<int> GetReachableStates(int from) const;

    /**
     * Debug: print graph structure.
     */
    void PrintGraph() const;

private:
    // Edges keyed by (from, to) state pair
    std::map<std::pair<int, int>, GraphEdge> edges;

    // Nodes in the graph
    std::set<int> nodeSet;

    // Tarjan's SCC algorithm implementation
    struct TarjanState {
        int index = 0;
        std::map<int, int> indices;
        std::map<int, int> lowLinks;
        std::map<int, bool> onStack;
        std::vector<int> stack;
        std::vector<std::vector<int>> sccs;
    };

    void StrongConnect(
        int v,
        TarjanState& ts,
        const std::map<int, std::vector<int>>& adjList
    );

    /**
     * Build an adjacency list from the valid edges for SCC computation.
     * Movement types are grouped so SCC analysis is per-type (the paper
     * groups by label sets; here we group by locomotion vs. action states).
     */
    std::map<int, std::vector<int>> BuildAdjacencyList() const;
};
