#pragma once

#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <cstdint>
#include <cstring>

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
        std::cout << "[TFLitePredictor] ML weights loaded from " << weightsPath
                  << " (" << layers_.size() << " layers)\n";
        return modelLoaded ? TFLiteStatus::Ok : TFLiteStatus::Error;
    }

    /**
     * Run the forward pass. `input` must be exactly the first layer's input
     * size (80); `output` receives the last layer's output (100). Returns Ok
     * on success, Error otherwise (caller falls back to Kalman).
     */
    TFLiteStatus runInference(const std::vector<float>& input, std::vector<float>& output) {
        if (!modelLoaded || layers_.empty()) return TFLiteStatus::NotLoaded;
        if (input.size() != (size_t)layers_[0].in) return TFLiteStatus::Error;

        std::vector<float> act = input;
        for (size_t l = 0; l < layers_.size(); ++l) {
            const Layer& layer = layers_[l];
            const bool last = (l + 1 == layers_.size());
            std::vector<float> next((size_t)layer.out, 0.0f);

            const float* w = layer.weights.data();
            for (int o = 0; o < layer.out; ++o) {
                float sum = layer.bias[o];
                const float* row = w + (size_t)o * layer.in;
                for (int i = 0; i < layer.in; ++i)
                    sum += row[i] * act[(size_t)i];
                next[(size_t)o] = last ? sum : (sum > 0.0f ? sum : 0.0f);  // ReLU
            }
            act = std::move(next);
        }

        output = std::move(act);
        return TFLiteStatus::Ok;
    }

    void setPythonEnvPath(const std::string& path) {
        (void)path;  // retained for API compatibility; no Python is used
    }

    bool isModelLoaded() const { return modelLoaded; }

private:
    struct Layer {
        int in = 0;
        int out = 0;
        std::vector<float> bias;
        std::vector<float> weights;  // row-major [out][in]
    };

    std::vector<Layer> layers_;
    bool modelLoaded = false;
};
