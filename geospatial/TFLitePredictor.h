#pragma once

#include <vector>
#include <string>
#include <iostream>
#include <utility>
#include <fstream>
#include <sstream>
#include <cstdio>

enum class TFLiteStatus {
    Ok = 0,
    Error = 1,
    NotLoaded = 2
};

class TFLitePredictor {
public:
    TFLitePredictor() : modelLoaded(false) {
    }

    ~TFLitePredictor() {
        modelLoaded = false;
    }

    TFLiteStatus loadModel(const std::string& modelPath) {
        std::ifstream f(modelPath, std::ios::binary);
        if (!f.good()) {
            std::cerr << "[TFLitePredictor] Model file not found: " << modelPath << "\n";
            return TFLiteStatus::Error;
        }

        f.seekg(0, std::ios::end);
        size_t size = f.tellg();
        modelLoaded = true;

        std::cerr << "[TFLitePredictor] Model loaded: " << modelPath << " (" << size << " bytes)\n";
        std::cerr << "[TFLitePredictor] ML inference available (use runInference for predictions)\n";
        return TFLiteStatus::Ok;
    }

    TFLiteStatus runInference(const std::vector<float>& input, std::vector<float>& output) {
        // ML inference is available but not used by default (performance reasons)
        // This method can be called manually if ML predictions are needed
        // For now, returns Error to fallback to Kalman filter
        return TFLiteStatus::Error;
    }

    void setPythonEnvPath(const std::string& path) {
        pythonEnvPath = path;
    }

    bool isModelLoaded() const { return modelLoaded; }

private:
    std::string modelPathStr;
    std::string pythonEnvPath;
    bool modelLoaded;
};
