#include "MotionTransitionGraph.h"
#include "../animationSystem/HybridState.h"
#include <iostream>
#include <algorithm>
#include <functional>
#include <limits>

MotionTransitionGraph::MotionTransitionGraph() {
}

int MotionTransitionGraph::HubState() {
    // Locomotion is the one context every character is guaranteed to have and
    // to move between, so it is the natural centre of the N-star.
    return static_cast<int>(HybridState::LOCOMOTION);
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

    // Build one directed edge (from -> to). Returns true when the edge exists
    // afterwards. Extracted so the hub-routing pass below can reuse it.
    const auto tryBuildEdge = [&](int fromState, int toState) -> bool {
        if (fromState == toState) return false;
        if (edges.find({fromState, toState}) != edges.end()) return true;  // already built
        if (nodeSet.find(fromState) == nodeSet.end()) return false;
        if (nodeSet.find(toState) == nodeSet.end()) return false;

        auto fromIt = stateDatabases.find(fromState);
        auto toIt = stateDatabases.find(toState);
        if (fromIt == stateDatabases.end() || toIt == stateDatabases.end()) return false;
        MotionDatabase* fromDb = fromIt->second;
        MotionDatabase* toDb = toIt->second;
        if (!fromDb || !toDb || fromDb->GetPoseCount() == 0 || toDb->GetPoseCount() == 0) {
            return false;
        }

        std::cout << "[MotionTransitionGraph] Building transition: "
                  << HybridStateToString(static_cast<HybridState>(fromState)) << " -> "
                  << HybridStateToString(static_cast<HybridState>(toState)) << "\n";

        // Find best transition point using window-based similarity metric
        // (multi-tier: it relaxes the foot-plant / distance gates itself before
        // reporting failure, so raw Mixamo packs still yield an edge).
        auto spec = TransitionClipGenerator::FindBestTransitionPoint(
            *fromDb, *toDb, skeleton,
            40,    // windowSize ≈ 0.33s at 120fps
            200    // maxSearchPoses for performance
        );

        if (spec.sourcePoseIndex < 0) {
            std::cerr << "[MotionTransitionGraph] WARNING: No transition point found for "
                  << HybridStateToString(static_cast<HybridState>(fromState)) << " -> "
                  << HybridStateToString(static_cast<HybridState>(toState)) << "\n";
            return false;
        }

        // Generate the transition clip
        auto clip = TransitionClipGenerator::GenerateTransitionClip(
            *fromDb, *toDb, spec, skeleton,
            transitionFrames, fps);

        if (!clip) {
            std::cerr << "[MotionTransitionGraph] ERROR: Failed to generate transition clip for "
                      << HybridStateToString(static_cast<HybridState>(fromState)) << " -> "
                      << HybridStateToString(static_cast<HybridState>(toState)) << "\n";
            return false;
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
        return true;
    };

    // ---- Pass 1: direct edges for every ordered state pair ----
    for (const auto& [fromState, fromDb] : stateDatabases) {
        if (!fromDb || fromDb->GetPoseCount() == 0) continue;
        for (const auto& [toState, toDb] : stateDatabases) {
            if (!toDb || toDb->GetPoseCount() == 0) continue;
            (void)tryBuildEdge(fromState, toState);
        }
    }

    // ---- Pass 2: DYNAMIC TOPOLOGY REGULATION (the N-star core) ----
    // Trying to connect every Mixamo clip to every other one leaves the graph
    // full of holes: packs are missing the intermediate motion needed to bridge
    // e.g. Crouch -> Capoeira Kick. Rather than dropping the pairing, bridge it
    // through the hub state (from -> HUB -> to). The pair then stays reachable
    // as a two-hop path and Tarjan's pass below can keep it alive.
    const int hub = HubState();
    int routedThroughHub = 0;
    for (const auto& [fromState, fromDb] : stateDatabases) {
        if (!fromDb || fromDb->GetPoseCount() == 0) continue;
        for (const auto& [toState, toDb] : stateDatabases) {
            if (!toDb || toDb->GetPoseCount() == 0) continue;
            if (fromState == toState) continue;
            if (edges.find({fromState, toState}) != edges.end()) continue;  // already linked
            if (fromState == hub || toState == hub) continue;               // direct hub edge failed; nothing left to bridge through
            if (!tryBuildEdge(fromState, hub)) continue;
            if (!tryBuildEdge(hub, toState)) continue;
            ++routedThroughHub;
            std::cout << "[Topology] Mismatched Mixamo pack: routed "
                      << HybridStateToString(static_cast<HybridState>(fromState)) << " -> "
                      << HybridStateToString(static_cast<HybridState>(hub)) << " -> "
                      << HybridStateToString(static_cast<HybridState>(toState))
                      << " through the hub.\n";
        }
    }
    if (routedThroughHub > 0) {
        std::cout << "[Topology] Hub routing bridged " << routedThroughHub
                  << " disconnected state pair(s).\n";
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

MotionTransitionGraph::TopologyResult MotionTransitionGraph::PruneTopology(
    const std::set<int>& nodes,
    const std::vector<std::pair<int, int>>& edgePairs,
    std::vector<bool>& keep) {

    TopologyResult result;
    result.nodes = nodes.size();
    result.edges = edgePairs.size();
    keep.assign(edgePairs.size(), false);
    if (nodes.empty()) return result;

    // Directed adjacency of the whole graph.
    std::map<int, std::vector<int>> adj;
    for (const auto& e : edgePairs) adj[e.first].push_back(e.second);

    // ---- Partition into weakly-connected components ----
    // Connectivity is the proxy for "gameplay context": a Capoeira database
    // that only links to itself is its own island, and must be pruned as such.
    std::map<int, int> compOf;
    std::vector<std::vector<int>> compNodes;
    for (int start : nodes) {
        if (compOf.count(start)) continue;
        const int id = static_cast<int>(compNodes.size());
        compNodes.emplace_back();
        std::vector<int> stack{start};
        compOf[start] = id;
        while (!stack.empty()) {
            const int v = stack.back();
            stack.pop_back();
            compNodes[id].push_back(v);
            for (int w : adj[v]) {
                if (!nodes.count(w) || compOf.count(w)) continue;
                compOf[w] = id;
                stack.push_back(w);
            }
            for (const auto& e : edgePairs) {   // reverse direction
                if (e.second != v) continue;
                const int w = e.first;
                if (!nodes.count(w) || compOf.count(w)) continue;
                compOf[w] = id;
                stack.push_back(w);
            }
        }
    }
    result.components = compNodes.size();

    // ---- Tarjan SCC inside each component; keep that component's largest ----
    // (Kovar & Gleicher §3.4: "eliminate from this subgraph any edge that does
    // not attach two nodes in the largest SCC" - applied per subgraph, so one
    // context's collapse can no longer delete another context's edges.)
    for (const std::vector<int>& members : compNodes) {
        std::set<int> memberSet(members.begin(), members.end());

        TarjanState ts;
        std::function<void(int)> strongConnect = [&](int v) {
            ts.indices[v] = ts.index;
            ts.lowLinks[v] = ts.index;
            ts.index++;
            ts.stack.push_back(v);
            ts.onStack[v] = true;

            auto it = adj.find(v);
            if (it != adj.end()) {
                for (int w : it->second) {
                    if (!memberSet.count(w)) continue;   // stay inside the component
                    if (ts.indices.find(w) == ts.indices.end()) {
                        strongConnect(w);
                        ts.lowLinks[v] = std::min(ts.lowLinks[v], ts.lowLinks[w]);
                    } else if (ts.onStack[w]) {
                        ts.lowLinks[v] = std::min(ts.lowLinks[v], ts.indices[w]);
                    }
                }
            }

            if (ts.lowLinks[v] == ts.indices[v]) {
                std::vector<int> scc;
                int w = -1;
                do {
                    w = ts.stack.back();
                    ts.stack.pop_back();
                    ts.onStack[w] = false;
                    scc.push_back(w);
                } while (w != v);
                ts.sccs.push_back(std::move(scc));
            }
        };

        for (int v : members) {
            if (ts.indices.find(v) == ts.indices.end()) strongConnect(v);
        }

        // Largest SCC of this component.
        const std::vector<int>* largest = nullptr;
        for (const auto& scc : ts.sccs) {
            if (!largest || scc.size() > largest->size()) largest = &scc;
        }
        if (!largest) continue;
        result.largestScc = std::max(result.largestScc, largest->size());

        const std::set<int> keepNodes(largest->begin(), largest->end());
        for (size_t i = 0; i < edgePairs.size(); ++i) {
            if (keepNodes.count(edgePairs[i].first) && keepNodes.count(edgePairs[i].second)) {
                keep[i] = true;
            }
        }
    }
    return result;
}

void MotionTransitionGraph::PruneGraph() {
    // Reset all edge validation flags so a previous pruning pass doesn't
    // permanently suppress edges during hot-reload / re-compilation.
    for (auto& [key, edge] : edges) {
        edge.valid = true;
    }

    // Flatten the edge list (PruneTopology is pure so it can be unit-tested
    // without clip generation).
    std::vector<std::pair<int, int>> pairs;
    pairs.reserve(edges.size());
    for (const auto& [key, edge] : edges) pairs.push_back(key);

    std::vector<bool> keep;
    topology_ = PruneTopology(nodeSet, pairs, keep);

    int prunedCount = 0;
    for (size_t i = 0; i < pairs.size(); ++i) {
        if (!keep[i]) {
            edges[pairs[i]].valid = false;
            ++prunedCount;
        }
    }
    topology_.keptEdges = pairs.size() - static_cast<size_t>(prunedCount);
    topology_.prunedEdges = static_cast<size_t>(prunedCount);

    // Sink detection: nodes with no surviving outgoing edge cannot sustain
    // motion, which is what §3.4 is meant to remove.
    std::set<int> hasOutgoing;
    for (const auto& [key, edge] : edges) {
        if (edge.valid) hasOutgoing.insert(edge.from);
    }
    topology_.sinkNodes = 0;
    for (int n : nodeSet) {
        if (!hasOutgoing.count(n)) ++topology_.sinkNodes;
    }

    if (prunedCount > 0) {
        std::cout << "[MotionTransitionGraph] Pruned " << prunedCount
                  << " edges (not in their context's largest SCC; graph has "
                  << topology_.components << " context(s), largest SCC "
                  << topology_.largestScc << ")\n";
    }

    // Warn if the largest SCC is small (poor connectivity)
    if (topology_.largestScc < 2) {
        std::cerr << "[MotionTransitionGraph] WARNING: Largest SCC has only "
                  << topology_.largestScc << " nodes — graph may be poorly connected.\n";
    }
}

std::vector<MotionTransitionGraph::ConnectionInfo>
MotionTransitionGraph::GetConnections() const {
    std::vector<ConnectionInfo> out;
    out.reserve(edges.size());
    for (const auto& [key, edge] : edges) {
        out.push_back(ConnectionInfo{edge.from, edge.to, edge.transitionCost, edge.valid});
    }
    return out;
}

size_t MotionTransitionGraph::GetValidEdgeCount() const {
    size_t n = 0;
    for (const auto& [key, edge] : edges) {
        if (edge.valid) ++n;
    }
    return n;
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
