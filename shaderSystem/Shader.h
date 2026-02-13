#ifndef SHADER_H
#define SHADER_H

#include <string>
#include <glm/glm.hpp>

// ------------------------------------------------------------
// Shader Class
// ------------------------------------------------------------
class Shader
{
public:
    unsigned int ID;

    // Constructor
    Shader(const char* vertexPath, const char* fragmentPath);

    // Use program
    void use() const;

    // Uniform helpers
    void setBool (const std::string& name, bool value) const;
    void setInt  (const std::string& name, int value) const;
    void setFloat(const std::string& name, float value) const;
    void setVec3 (const std::string& name, const glm::vec3& value) const;
    void setMat4 (const std::string& name, const glm::mat4& mat) const;

private:
    // Internal error checking
    void checkCompileErrors(unsigned int shader, const std::string& type);
};

#endif // SHADER_H

