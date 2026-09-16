#pragma once
#include <string>

// Generates a procedural pine tree (tapered trunk + layered cone tiers) as an
// OBJ/MTL pair under <assetDir>/pine/. The project ships no pine asset, so this
// "creates the asset from whatever is required" using geometry alone and reusing
// the existing coast_rocks texture for bark + the grass texture for needles.
// Returns the OBJ path (empty string on failure). Loading goes through the
// normal Model pipeline, so the tree picks up the same normal/roughness/AO
// binding + canonical sun lighting as every other world object.
std::string generateProceduralPine(const std::string& assetDir);
