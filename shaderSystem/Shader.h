#ifndef SHADER_H
#define SHADER_H

#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include <glad/glad.h>

// ------------------------------------------------------------
// Shader Class
// ------------------------------------------------------------
class Shader
{
public:
    unsigned int ID;

    // Constructor (from file paths)
    Shader(const char* vertexPath, const char* fragmentPath);

    // Constructor (from file paths, 4 stages: vertex + tessellation control +
    // tessellation evaluation + fragment). Falls back gracefully (ID == 0) if
    // any stage fails to compile or the program fails to link.
    Shader(const char* vertexPath, const char* tessCtrlPath,
           const char* tessEvalPath, const char* fragmentPath);

    // Constructor (from raw source strings)
    Shader(const std::string& vertexSource, const std::string& fragmentSource, bool fromString);

    // Use program
    void use() const;

    // Uniform helpers
    void setBool (const std::string& name, bool value) const;
    void setInt  (const std::string& name, int value) const;
    void setFloat(const std::string& name, float value) const;
    void setVec3 (const std::string& name, const glm::vec3& value) const;
    void setMat4 (const std::string& name, const glm::mat4& mat) const;

    // Get cached uniform location
    GLint getUniformLocation(const std::string& name) const;

private:
    // Internal error checking
    void checkCompileErrors(unsigned int shader, const std::string& type);

    // Cached uniform locations (populated on first use)
    mutable std::unordered_map<std::string, GLint> m_uniformLocations;
};

#endif // SHADER_H

