#include "MotionTransitionGraph.h"
#include "../animationSystem/HybridState.h"
#include <iostream>
#include <algorithm>
#include <functional>
#include <limits>

MotionTransitionGraph::MotionTransitionGraph() {
}

void MotionTransitionGraph::BuildTransitions(
    const std::map<int, MotionDatabase*>& stateDatabases,
    const Skeleton* skeleton,
    int transitionFrames,
    float fps) {

    if (!skeleton) return;

    // Collect all states that have databases
    for (const auto& [state, db] : stateDatabases) {
        if (db && db->GetPoseCount() > 0) {
            nodeSet.insert(state);
        }
    }

    // Generate transition clips for all ordered state pairs
    for (const auto& [fromState, fromDb] : stateDatabases) {
        if (!fromDb || fromDb->GetPoseCount() == 0) continue;
        if (nodeSet.find(fromState) == nodeSet.end()) continue;

        for (const auto& [toState, toDb] : stateDatabases) {
            if (!toDb || toDb->GetPoseCount() == 0) continue;
            if (nodeSet.find(toState) == nodeSet.end()) continue;
            if (fromState == toState) continue;  // No self-transitions

            std::cout << "[MotionTransitionGraph] Building transition: "
                      << HybridStateToString(static_cast<HybridState>(fromState)) << " -> "
                      << HybridStateToString(static_cast<HybridState>(toState)) << "\n";

            // Find best transition point using window-based similarity metric
            auto spec = TransitionClipGenerator::FindBestTransitionPoint(
                *fromDb, *toDb, skeleton,
                40,    // windowSize ≈ 0.33s at 120fps
                200    // maxSearchPoses for performance
            );

            if (spec.sourcePoseIndex < 0) {
                std::cerr << "[MotionTransitionGraph] WARNING: No transition point found for "
                      << HybridStateToString(static_cast<HybridState>(fromState)) << " -> "
                      << HybridStateToString(static_cast<HybridState>(toState)) << "\n";
                continue;
            }

            // Generate the transition clip
            auto clip = TransitionClipGenerator::GenerateTransitionClip(
                *fromDb, *toDb, spec, skeleton,
                transitionFrames, fps);

            if (!clip) {
                std::cerr << "[MotionTransitionGraph] ERROR: Failed to generate transition clip for "
                          << HybridStateToString(static_cast<HybridState>(fromState)) << " -> "
                          << HybridStateToString(static_cast<HybridState>(toState)) << "\n";
                continue;
            }

            GraphEdge edge;
            edge.from = fromState;
            edge.to = toState;
            edge.transitionClip = clip;
            edge.transitionCost = spec.distance;
            edge.valid = true;

            edges[{fromState, toState}] = edge;
            std::cout << "[MotionTransitionGraph] Transition built: "
                      << HybridStateToString(static_cast<HybridState>(fromState)) << " -> "
                      << HybridStateToString(static_cast<HybridState>(toState))
                      << " (distance=" << spec.distance << ")\n";
        }
    }
}

std::map<int, std::vector<int>>
MotionTransitionGraph::BuildAdjacencyList() const {
    std::map<int, std::vector<int>> adjList;

    for (const auto& [key, edge] : edges) {
        if (!edge.valid) continue;
        adjList[edge.from].push_back(edge.to);
    }

    return adjList;
}

void MotionTransitionGraph::StrongConnect(
    int v,
    TarjanState& ts,
    const std::map<int, std::vector<int>>& adjList) {

    ts.indices[v] = ts.index;
    ts.lowLinks[v] = ts.index;
    ts.index++;
    ts.stack.push_back(v);
    ts.onStack[v] = true;

    auto it = adjList.find(v);
    if (it != adjList.end()) {
        for (int w : it->second) {
            if (ts.indices.find(w) == ts.indices.end()) {
                StrongConnect(w, ts, adjList);
                ts.lowLinks[v] = std::min(ts.lowLinks[v], ts.lowLinks[w]);
            } else if (ts.onStack[w]) {
                ts.lowLinks[v] = std::min(ts.lowLinks[v], ts.indices[w]);
            }
        }
    }

    // If v is a root node, pop the stack and generate an SCC
    if (ts.lowLinks[v] == ts.indices[v]) {
        std::vector<int> scc;
        int w;
        do {
            w = ts.stack.back();
            ts.stack.pop_back();
            ts.onStack[w] = false;
            scc.push_back(w);
        } while (w != v);
        ts.sccs.push_back(scc);
    }
}

void MotionTransitionGraph::PruneGraph() {
    // Build adjacency list from valid edges
    auto adjList = BuildAdjacencyList();

    // Run Tarjan's SCC algorithm
    TarjanState ts;
    for (int v : nodeSet) {
        if (ts.indices.find(v) == ts.indices.end()) {
            StrongConnect(v, ts, adjList);
        }
    }

    if (ts.sccs.empty()) {
        return;
    }

    // Find the largest SCC
    size_t largestSccIdx = 0;
    size_t largestSccSize = 0;
    for (size_t i = 0; i < ts.sccs.size(); ++i) {
        if (ts.sccs[i].size() > largestSccSize) {
            largestSccSize = ts.sccs[i].size();
            largestSccIdx = i;
        }
    }

    // Build a set of nodes in the largest SCC
    std::set<int> largestSccNodes(ts.sccs[largestSccIdx].begin(),
                                           ts.sccs[largestSccIdx].end());

    // Mark edges to/from nodes not in the largest SCC as pruned
    int prunedCount = 0;
    for (auto& [key, edge] : edges) {
        if (!edge.valid) continue;

        // An edge is kept only if both endpoints are in the largest SCC
        // (Kovar & Gleicher §3.4: "eliminate from this subgraph any edge that
        // does not attach two nodes in the largest SCC")
        if (largestSccNodes.find(edge.from) == largestSccNodes.end() ||
            largestSccNodes.find(edge.to) == largestSccNodes.end()) {
            edge.valid = false;
            prunedCount++;
        }
    }

    if (prunedCount > 0) {
        std::cout << "[MotionTransitionGraph] Pruned " << prunedCount
                  << " edges (not in largest SCC of size " << largestSccSize << ")\n";
    }

    // Warn if the largest SCC is small (poor connectivity)
    if (largestSccSize < 2) {
        std::cerr << "[MotionTransitionGraph] WARNING: Largest SCC has only "
                  << largestSccSize << " nodes — graph may be poorly connected.\n";
    }
}

std::shared_ptr<Animation> MotionTransitionGraph::GetTransition(
    int from, int to) const {

    auto it = edges.find({from, to});
    if (it == edges.end() || !it->second.valid) {
        return nullptr;
    }
    return it->second.transitionClip;
}

bool MotionTransitionGraph::HasTransition(int from, int to) const {
    auto it = edges.find({from, to});
    return it != edges.end() && it->second.valid;
}

float MotionTransitionGraph::GetTransitionCost(int from, int to) const {
    auto it = edges.find({from, to});
    if (it == edges.end() || !it->second.valid) {
        return std::numeric_limits<float>::max();
    }
    return it->second.transitionCost;
}

std::vector<const MotionTransitionGraph::GraphEdge*>
MotionTransitionGraph::GetOutgoingEdges(int from) const {
    std::vector<const GraphEdge*> result;
    for (const auto& [key, edge] : edges) {
        if (edge.from == from && edge.valid) {
            result.push_back(&edge);
        }
    }
    return result;
}

std::vector<int> MotionTransitionGraph::GetReachableStates(int from) const {
    std::vector<int> result;
    for (const auto& [key, edge] : edges) {
        if (edge.from == from && edge.valid) {
            result.push_back(edge.to);
        }
    }
    return result;
}

void MotionTransitionGraph::PrintGraph() const {
    std::cout << "=== MotionTransitionGraph ===\n";
    std::cout << "Nodes: " << nodeSet.size() << "\n";
    std::cout << "Edges: " << edges.size() << "\n";
    for (const auto& [key, edge] : edges) {
        std::cout << "  " << HybridStateToString(static_cast<HybridState>(edge.from)) << " -> "
                  << HybridStateToString(static_cast<HybridState>(edge.to))
                  << " [cost=" << edge.transitionCost
                  << ", valid=" << (edge.valid ? "yes" : "NO(pruned)")
                  << "]\n";
    }
}
