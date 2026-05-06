#include "Animation.h"
#include <algorithm>
#include <cmath>
#include <iostream>

// ---------------- Animation ----------------

Animation::Animation(const std::string &name, float duration, float tps)
    : name(name), duration(duration), ticksPerSecond(tps) {}

void Animation::AddBoneAnimation(const BoneAnimation &boneAnim)
{
    boneAnimations[boneAnim.boneName] = boneAnim;
}

void Animation::DebugPrintBoneNames() const
{
    std::cout << "==== Animation Bone Channels ====\n";
    for (const auto &[name, boneAnim] : boneAnimations)
    {
        std::cout << name << " | ";
        if (boneAnim.HasPositionAnimation())
            std::cout << "POS (" << boneAnim.positionTimes.size() << ") ";
        if (boneAnim.HasRotationAnimation())
            std::cout << "ROT (" << boneAnim.rotationTimes.size() << ") ";
        if (boneAnim.HasScaleAnimation())
            std::cout << "SCALE (" << boneAnim.scaleTimes.size() << ") ";
        std::cout << "\n";
    }
}

const BoneAnimation *Animation::GetBoneAnimation(const std::string &boneName) const
{
    auto it = boneAnimations.find(boneName);
    if (it == boneAnimations.end())
        return nullptr;
    return &it->second;
}

float Animation::GetDuration() const
{
    return duration;
}

float Animation::GetTicksPerSecond() const
{
    return ticksPerSecond;
}

size_t Animation::GetTotalKeyframeCount() const
{
    size_t count = 0;
    for (const auto &[_, bone] : boneAnimations)
    {
        count += bone.positionTimes.size();
        count += bone.rotationTimes.size();
        count += bone.scaleTimes.size();
    }
    return count;
}

// Helper: find insertion point in sorted arrays
static int FindKeyIndex(const std::vector<double> &times, double time)
{
    if (times.size() < 2)
        return 0;

    for (int i = 0; i < (int)times.size() - 1; ++i)
    {
        if (time < times[i + 1])
            return i;
    }
    return (int)times.size() - 2;
}

glm::vec3 BoneAnimation::InterpolatePosition(float time) const
{
    if (positionTimes.size() == 0)
        return glm::vec3(0.0f);
    if (positionTimes.size() == 1)
        return positionValues[0];

    int i = FindKeyIndex(positionTimes, (double)time);
    double t1 = positionTimes[i];
    double t2 = positionTimes[i + 1];
    double delta = t2 - t1;
    float blend = delta > 0.0001 ? (float)((time - t1) / delta) : 0.0f;

    return glm::mix(positionValues[i], positionValues[i + 1], blend);
}

glm::quat BoneAnimation::InterpolateRotation(float time) const
{
    if (rotationTimes.size() == 0)
        return glm::quat(1, 0, 0, 0);
    if (rotationTimes.size() == 1)
        return glm::normalize(rotationValues[0]);

    int i = FindKeyIndex(rotationTimes, (double)time);
    double t1 = rotationTimes[i];
    double t2 = rotationTimes[i + 1];
    double delta = t2 - t1;
    float blend = delta > 0.0001 ? (float)((time - t1) / delta) : 0.0f;

    glm::quat q1 = rotationValues[i];
    glm::quat q2 = rotationValues[i + 1];

    if (glm::dot(q1, q2) < 0.0f)
        q2 = -q2;

    glm::quat result = glm::slerp(q1, q2, blend);
    return glm::normalize(result);
}

glm::vec3 BoneAnimation::InterpolateScale(float time) const
{
    if (scaleTimes.size() == 0)
        return glm::vec3(1.0f);
    if (scaleTimes.size() == 1)
        return scaleValues[0];

    int i = FindKeyIndex(scaleTimes, (double)time);
    double t1 = scaleTimes[i];
    double t2 = scaleTimes[i + 1];
    double delta = t2 - t1;
    float blend = delta > 0.0001 ? (float)((time - t1) / delta) : 0.0f;

    return glm::mix(scaleValues[i], scaleValues[i + 1], blend);
}

void Animation::Compress(float positionTolerance, float rotationTolerance, float scaleTolerance)
{
    for (auto& [name, boneAnim] : boneAnimations)
    {
        // Reduce position keyframes
        if (boneAnim.positionTimes.size() > 2) {
            std::vector<double> newPosTimes;
            std::vector<glm::vec3> newPosValues;
            
            newPosTimes.push_back(boneAnim.positionTimes[0]);
            newPosValues.push_back(boneAnim.positionValues[0]);
            
            for (size_t i = 1; i < boneAnim.positionTimes.size() - 1; ++i) {
                // Simple keyframe reduction - check if removing this keyframe affects the interpolated value significantly
                float prevTime = static_cast<float>(boneAnim.positionTimes[i-1]);
                float currTime = static_cast<float>(boneAnim.positionTimes[i]);
                float nextTime = static_cast<float>(boneAnim.positionTimes[i+1]);
                
                glm::vec3 prevVal = boneAnim.positionValues[i-1];
                glm::vec3 currVal = boneAnim.positionValues[i];
                glm::vec3 nextVal = boneAnim.positionValues[i+1];
                
                // Linear interpolation between prev and next
                float blend = (currTime - prevTime) / (nextTime - prevTime);
                glm::vec3 interpolated = prevVal + (nextVal - prevVal) * blend;
                
                if (glm::distance(currVal, interpolated) > positionTolerance) {
                    // Keep this keyframe if it differs significantly from linear interpolation
                    newPosTimes.push_back(boneAnim.positionTimes[i]);
                    newPosValues.push_back(boneAnim.positionValues[i]);
                }
            }
            
            newPosTimes.push_back(boneAnim.positionTimes.back());
            newPosValues.push_back(boneAnim.positionValues.back());
            
            boneAnim.positionTimes = newPosTimes;
            boneAnim.positionValues = newPosValues;
        }
        
        // Reduce rotation keyframes
        if (boneAnim.rotationTimes.size() > 2) {
            std::vector<double> newRotTimes;
            std::vector<glm::quat> newRotValues;
            
            newRotTimes.push_back(boneAnim.rotationTimes[0]);
            newRotValues.push_back(boneAnim.rotationValues[0]);
            
            for (size_t i = 1; i < boneAnim.rotationTimes.size() - 1; ++i) {
                float prevTime = static_cast<float>(boneAnim.rotationTimes[i-1]);
                float currTime = static_cast<float>(boneAnim.rotationTimes[i]);
                float nextTime = static_cast<float>(boneAnim.rotationTimes[i+1]);
                
                glm::quat prevVal = boneAnim.rotationValues[i-1];
                glm::quat currVal = boneAnim.rotationValues[i];
                glm::quat nextVal = boneAnim.rotationValues[i+1];
                
                // Slerp between prev and next
                float blend = (currTime - prevTime) / (nextTime - prevTime);
                glm::quat interpolated = glm::slerp(prevVal, nextVal, blend);
                
                // Compare rotations using dot product (measure of similarity)
                float dotProduct = glm::abs(glm::dot(currVal, interpolated));
                if (dotProduct < (1.0f - rotationTolerance)) {
                    // Keep this keyframe if it differs significantly from slerp interpolation
                    newRotTimes.push_back(boneAnim.rotationTimes[i]);
                    newRotValues.push_back(boneAnim.rotationValues[i]);
                }
            }
            
            newRotTimes.push_back(boneAnim.rotationTimes.back());
            newRotValues.push_back(boneAnim.rotationValues.back());
            
            boneAnim.rotationTimes = newRotTimes;
            boneAnim.rotationValues = newRotValues;
        }
        
        // Reduce scale keyframes
        if (boneAnim.scaleTimes.size() > 2) {
            std::vector<double> newScaleTimes;
            std::vector<glm::vec3> newScaleValues;
            
            newScaleTimes.push_back(boneAnim.scaleTimes[0]);
            newScaleValues.push_back(boneAnim.scaleValues[0]);
            
            for (size_t i = 1; i < boneAnim.scaleTimes.size() - 1; ++i) {
                float prevTime = static_cast<float>(boneAnim.scaleTimes[i-1]);
                float currTime = static_cast<float>(boneAnim.scaleTimes[i]);
                float nextTime = static_cast<float>(boneAnim.scaleTimes[i+1]);
                
                glm::vec3 prevVal = boneAnim.scaleValues[i-1];
                glm::vec3 currVal = boneAnim.scaleValues[i];
                glm::vec3 nextVal = boneAnim.scaleValues[i+1];
                
                // Linear interpolation between prev and next
                float blend = (currTime - prevTime) / (nextTime - prevTime);
                glm::vec3 interpolated = prevVal + (nextVal - prevVal) * blend;
                
                if (glm::distance(currVal, interpolated) > scaleTolerance) {
                    // Keep this keyframe if it differs significantly from linear interpolation
                    newScaleTimes.push_back(boneAnim.scaleTimes[i]);
                    newScaleValues.push_back(boneAnim.scaleValues[i]);
                }
            }
            
            newScaleTimes.push_back(boneAnim.scaleTimes.back());
            newScaleValues.push_back(boneAnim.scaleValues.back());
            
            boneAnim.scaleTimes = newScaleTimes;
            boneAnim.scaleValues = newScaleValues;
        }
    }
    
    // Update compression flag and ratio
    isCompressed = true;
    size_t originalSize = GetTotalKeyframeCount();
    size_t compressedSize = GetCompressedSize();
    compressionRatio = (originalSize > 0) ? (float)compressedSize / (float)originalSize : 1.0f;
}

void Animation::ReduceKeyframes(float tolerance)
{
    Compress(tolerance, tolerance, tolerance);
}

size_t Animation::GetCompressedSize() const
{
    size_t count = 0;
    for (const auto &[_, bone] : boneAnimations)
    {
        count += bone.positionTimes.size();
        count += bone.rotationTimes.size();
        count += bone.scaleTimes.size();
    }
    return count;
}

size_t Animation::GetOriginalKeyframeCount() const
{
    return originalKeyframeCount > 0 ? originalKeyframeCount : GetTotalKeyframeCount();
}

float Animation::GetCompressionRatio() const
{
    size_t original = GetOriginalKeyframeCount();
    size_t compressed = GetCompressedSize();
    return (original > 0) ? static_cast<float>(compressed) / static_cast<float>(original) : 1.0f;
}

void Animation::RemoveConstantChannels(float tolerance)
{
    for (auto it = boneAnimations.begin(); it != boneAnimations.end(); ) {
        auto& [name, boneAnim] = *it;
        bool hasConstantPos = false;
        bool hasConstantRot = false;
        bool hasConstantScale = false;
        
        // Check if position channel is constant
        if (boneAnim.positionValues.size() > 1) {
            hasConstantPos = true;
            const auto& firstPos = boneAnim.positionValues[0];
            for (size_t i = 1; i < boneAnim.positionValues.size(); ++i) {
                if (glm::distance(firstPos, boneAnim.positionValues[i]) > tolerance) {
                    hasConstantPos = false;
                    break;
                }
            }
        }
        
        // Check if rotation channel is constant
        if (boneAnim.rotationValues.size() > 1) {
            hasConstantRot = true;
            const auto& firstRot = boneAnim.rotationValues[0];
            for (size_t i = 1; i < boneAnim.rotationValues.size(); ++i) {
                float dot = glm::abs(glm::dot(firstRot, boneAnim.rotationValues[i]));
                if (dot < (1.0f - tolerance)) {
                    hasConstantRot = false;
                    break;
                }
            }
        }
        
        // Check if scale channel is constant
        if (boneAnim.scaleValues.size() > 1) {
            hasConstantScale = true;
            const auto& firstScale = boneAnim.scaleValues[0];
            for (size_t i = 1; i < boneAnim.scaleValues.size(); ++i) {
                if (glm::distance(firstScale, boneAnim.scaleValues[i]) > tolerance) {
                    hasConstantScale = false;
                    break;
                }
            }
        }
        
        // Collapse constant channels to single keyframe
        if (hasConstantPos && boneAnim.positionValues.size() > 1) {
            boneAnim.positionTimes = {boneAnim.positionTimes[0]};
            boneAnim.positionValues = {boneAnim.positionValues[0]};
        }
        if (hasConstantRot && boneAnim.rotationValues.size() > 1) {
            boneAnim.rotationTimes = {boneAnim.rotationTimes[0]};
            boneAnim.rotationValues = {boneAnim.rotationValues[0]};
        }
        if (hasConstantScale && boneAnim.scaleValues.size() > 1) {
            boneAnim.scaleTimes = {boneAnim.scaleTimes[0]};
            boneAnim.scaleValues = {boneAnim.scaleValues[0]};
        }
        
        ++it;
    }
}

void Animation::QuantizeTranslations(uint8_t bits)
{
    if (bits < 8 || bits > 32) return;
    
    float maxVal = (1 << bits) - 1;
    float invMaxVal = 1.0f / maxVal;
    
    for (auto& [name, boneAnim] : boneAnimations) {
        // Quantize and dequantize position values
        for (auto& pos : boneAnim.positionValues) {
            pos.x = std::round(pos.x * maxVal) * invMaxVal;
            pos.y = std::round(pos.y * maxVal) * invMaxVal;
            pos.z = std::round(pos.z * maxVal) * invMaxVal;
        }
    }
}

void Animation::QuantizeRotations(uint8_t bits)
{
    if (bits < 8 || bits > 32) return;
    
    float maxVal = (1 << bits) - 1;
    float invMaxVal = 1.0f / maxVal;
    
    for (auto& [name, boneAnim] : boneAnimations) {
        for (auto& rot : boneAnim.rotationValues) {
            // Quantize each component
            float qx = std::round(rot.x * maxVal) * invMaxVal;
            float qy = std::round(rot.y * maxVal) * invMaxVal;
            float qz = std::round(rot.z * maxVal) * invMaxVal;
            float qw = std::round(rot.w * maxVal) * invMaxVal;
            
            // Re-normalize to ensure unit quaternion
            glm::quat quantized(qw, qx, qy, qz);
            rot = glm::normalize(quantized);
        }
    }
}

void Animation::QuantizeScales(uint8_t bits)
{
    if (bits < 8 || bits > 32) return;
    
    float maxVal = (1 << bits) - 1;
    float invMaxVal = 1.0f / maxVal;
    
    for (auto& [name, boneAnim] : boneAnimations) {
        for (auto& scale : boneAnim.scaleValues) {
            scale.x = std::round(scale.x * maxVal) * invMaxVal;
            scale.y = std::round(scale.y * maxVal) * invMaxVal;
            scale.z = std::round(scale.z * maxVal) * invMaxVal;
        }
    }
}

void Animation::FullCompression(float keyframeTolerance)
{
    // Store original keyframe count before compression
    if (originalKeyframeCount == 0) {
        originalKeyframeCount = GetTotalKeyframeCount();
    }
    
    // Step 1: Remove constant channels (biggest win for idle bones)
    RemoveConstantChannels(0.001f);
    
    // Step 2: Iterative keyframe reduction
    Compress(keyframeTolerance, keyframeTolerance * 0.5f, keyframeTolerance);
    
    // Step 3: Quantize values (enables better compression for storage)
    QuantizeTranslations(16);
    QuantizeRotations(14);
    QuantizeScales(16);
    
    // Update compression statistics
    isCompressed = true;
    compressionRatio = GetCompressionRatio();
}
