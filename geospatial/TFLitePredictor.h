#pragma once

#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <algorithm>

/**
 * TFLitePredictor - self-contained ML inference for trajectory prediction.
 *
 * The engine does NOT link the TensorFlow Lite runtime. Instead, the training
 * script (temp/create_tflite_model.py) exports the small MLP's weights to a
 * flat binary file (geospatial/trajectory_predict_weights.bin); this class
 * loads them and runs the forward pass directly. The .tflite file is still
 * written by the trainer for portability.
 *
 * Model: Input(80) -> Dense(64, relu) -> Dense(32, relu) -> Dense(100, linear)
 *   Input  : 20 history samples of (dx/100, dy/100, speed/10, heading/pi)
 *   Output : 50 future samples of (dx/100, dy/100) at 1 s steps
 *
 * Weights format (little-endian):
 *   "MLPW" | int32 version | int32 layerCount |
 *   per layer: int32 out | int32 in | float bias[out] | float weights[out*in]
 */
enum class TFLiteStatus {
    Ok = 0,
    Error = 1,
    NotLoaded = 2
};

class TFLitePredictor {
public:
    TFLitePredictor() = default;

    /**
     * Load the model. `modelPath` points at the .tflite file (kept for
     * compatibility); the weights file is derived by replacing the ".tflite"
     * suffix with "_weights.bin". Returns Ok only if the weights parsed.
     */
    TFLiteStatus loadModel(const std::string& modelPath) {
        modelLoaded = false;
        layers_.clear();

        std::string weightsPath = modelPath;
        const size_t dot = weightsPath.rfind(".tflite");
        if (dot != std::string::npos)
            weightsPath = weightsPath.substr(0, dot) + "_weights.bin";
        else
            weightsPath = modelPath + "_weights.bin";

        std::ifstream f(weightsPath, std::ios::binary);
        if (!f.good()) {
            std::cerr << "[TFLitePredictor] Weights file not found: " << weightsPath << "\n";
            return TFLiteStatus::Error;
        }

        char magic[4];
        f.read(magic, 4);
        if (std::memcmp(magic, "MLPW", 4) != 0) {
            std::cerr << "[TFLitePredictor] Bad magic in " << weightsPath << "\n";
            return TFLiteStatus::Error;
        }
        int32_t version = 0, layerCount = 0;
        f.read(reinterpret_cast<char*>(&version), 4);
        f.read(reinterpret_cast<char*>(&layerCount), 4);
        if (version != 1 || layerCount < 1 || layerCount > 8) {
            std::cerr << "[TFLitePredictor] Unsupported weights layout\n";
            return TFLiteStatus::Error;
        }

        for (int32_t l = 0; l < layerCount; ++l) {
            int32_t out = 0, in = 0;
            f.read(reinterpret_cast<char*>(&out), 4);
            f.read(reinterpret_cast<char*>(&in), 4);
            if (out <= 0 || in <= 0 || out > 4096 || in > 4096) {
                std::cerr << "[TFLitePredictor] Bad layer dims\n";
                return TFLiteStatus::Error;
            }
            Layer layer;
            layer.out = out;
            layer.in = in;
            layer.bias.resize(out);
            layer.weights.resize((size_t)out * (size_t)in);
            f.read(reinterpret_cast<char*>(layer.bias.data()), out * sizeof(float));
            f.read(reinterpret_cast<char*>(layer.weights.data()), out * (size_t)in * sizeof(float));
            layers_.push_back(std::move(layer));
        }

        modelLoaded = !layers_.empty();

        // Pre-size the allocation-free inference scratch pads to the widest
        // layer dimension (input OR output) so runInference() never allocates.
        // For the canonical model (80 -> 64 -> 32 -> 100) this is 100 floats;
        // every layer's input/output fits within [0, maxLayerWidth_].
        maxLayerWidth_ = 0;
        for (const auto& layer : layers_) {
            maxLayerWidth_ = std::max(maxLayerWidth_,
                std::max((size_t)layer.in, (size_t)layer.out));
        }
        buffer_A_.resize(maxLayerWidth_);
        buffer_B_.resize(maxLayerWidth_);
        if (!modelLoaded) maxLayerWidth_ = 0;  // keep state consistent on failure

        std::cout << "[TFLitePredictor] ML weights loaded from " << weightsPath
                  << " (" << layers_.size() << " layers, scratchpad=" << maxLayerWidth_ << " floats)\n";
        return modelLoaded ? TFLiteStatus::Ok : TFLiteStatus::Error;
    }

    /**
     * Run the forward pass. `input` must be exactly the first layer's input
     * size (80); `output` receives the last layer's output (100). Returns Ok
     * on success, Error otherwise (caller falls back to Kalman).
     *
     * Allocation-free on the hot path: the input is memcpy'd into the
     * pre-sized buffer_A_ scratch pad and each layer's activations ping-pong
     * between buffer_A_/buffer_B_ (a pointer swap per layer). No per-layer
     * std::vector is ever constructed here. The caller should pre-size
     * `output` once (PredictiveModel sizes its reusable buffer to
     * getOutputSize()) so even the final copy avoids allocation.
     */
    TFLiteStatus runInference(const std::vector<float>& input, std::vector<float>& output) {
        if (!modelLoaded || layers_.empty()) return TFLiteStatus::NotLoaded;
        if (input.size() != (size_t)layers_[0].in) return TFLiteStatus::Error;

        const size_t outputSize = (size_t)layers_.back().out;

        // Seed buffer_A with the incoming activations (memcpy, no allocation).
        std::memcpy(buffer_A_.data(), input.data(),
                    (size_t)layers_[0].in * sizeof(float));

        // Ping-pong: layer l reads from the buffer holding layer (l-1)'s output
        // and writes to the other scratch pad. Replaces the old per-layer
        // `std::vector<float> next(...)` construction entirely.
        float* readBuf  = buffer_A_.data();
        float* writeBuf = buffer_B_.data();

        for (size_t l = 0; l < layers_.size(); ++l) {
            const Layer& layer = layers_[l];
            const bool last = (l + 1 == layers_.size());

            const float* w = layer.weights.data();
            for (int o = 0; o < layer.out; ++o) {
                float sum = layer.bias[o];
                const float* row = w + (size_t)o * layer.in;
                for (int i = 0; i < layer.in; ++i)
                    sum += row[i] * readBuf[(size_t)i];
                // Linear activation on the final layer; ReLU on hidden layers.
                writeBuf[(size_t)o] = last ? sum : (sum > 0.0f ? sum : 0.0f);
            }

            // Swap read/write roles for the next layer (O(1) pointer swap).
            // After the final iteration `readBuf` aliases the just-filled output.
            std::swap(readBuf, writeBuf);
        }

        // Drain the final-layer result into the caller's vector. When the
        // caller pre-sized `output` (capacity >= outputSize), this is a
        // pure copy with no allocation.
        output.assign(readBuf, readBuf + outputSize);
        return TFLiteStatus::Ok;
    }

    void setPythonEnvPath(const std::string& path) {
        (void)path;  // retained for API compatibility; no Python is used
    }

    bool isModelLoaded() const { return modelLoaded; }

    // Dimensions of the loaded network. Callers (e.g. PredictiveModel's
    // predictWithML) can pre-size their reusable input/output buffers to these
    // so the entire inference hot path is allocation-free end-to-end.
    size_t getInputSize() const { return layers_.empty() ? 0 : (size_t)layers_[0].in; }
    size_t getOutputSize() const { return layers_.empty() ? 0 : (size_t)layers_.back().out; }

private:
    struct Layer {
        int in = 0;
        int out = 0;
        std::vector<float> bias;
        std::vector<float> weights;  // row-major [out][in]
    };

    std::vector<Layer> layers_;
    bool modelLoaded = false;

    // Allocation-free inference scratch pads (see forGeo.txt §1). Resized once
    // in loadModel() to maxLayerWidth_ (the widest layer dimension) and reused
    // via ping-pong on every runInference() call, so no per-layer/per-frame
    // heap allocation ever occurs on the real-time loop.
    size_t maxLayerWidth_ = 0;
    std::vector<float> buffer_A_;
    std::vector<float> buffer_B_;
};
