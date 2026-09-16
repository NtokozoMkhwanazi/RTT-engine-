#include "Shader.h"

#include <glad/glad.h>
#include <fstream>
#include <sstream>
#include <iostream>

// ------------------------------------------------------------
// Constructor
// ------------------------------------------------------------
Shader::Shader(const char* vertexPath, const char* fragmentPath)
    : ID(0)
{
    std::string vertexCode;
    std::string fragmentCode;

    std::ifstream vShaderFile;
    std::ifstream fShaderFile;

    // Ensure ifstream objects can throw exceptions
    vShaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    fShaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);

    try
    {
        // Open files
        vShaderFile.open(vertexPath);
        fShaderFile.open(fragmentPath);

        std::stringstream vShaderStream, fShaderStream;
        vShaderStream << vShaderFile.rdbuf();
        fShaderStream << fShaderFile.rdbuf();

        vShaderFile.close();
        fShaderFile.close();

        vertexCode   = vShaderStream.str();
        fragmentCode = fShaderStream.str();
    }
    catch (std::ifstream::failure&)
    {
        std::cerr << "ERROR::SHADER::FILE_NOT_SUCCESSFULLY_READ\n";
        std::cerr << "Vertex Path:   " << vertexPath << "\n";
        std::cerr << "Fragment Path: " << fragmentPath << "\n";
        return;
    }

    const char* vShaderCode = vertexCode.c_str();
    const char* fShaderCode = fragmentCode.c_str();

    // ------------------------------------------------------------
    // Compile Vertex Shader
    // ------------------------------------------------------------
    unsigned int vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &vShaderCode, nullptr);
    glCompileShader(vertex);
    checkCompileErrors(vertex, "VERTEX");

    // ------------------------------------------------------------
    // Compile Fragment Shader
    // ------------------------------------------------------------
    unsigned int fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, nullptr);
    glCompileShader(fragment);
    checkCompileErrors(fragment, "FRAGMENT");

    // ------------------------------------------------------------
    // Shader Program
    // ------------------------------------------------------------
    ID = glCreateProgram();
    glAttachShader(ID, vertex);
    glAttachShader(ID, fragment);
    glLinkProgram(ID);
    checkCompileErrors(ID, "PROGRAM");

    glDeleteShader(vertex);
    glDeleteShader(fragment);
}

Shader::Shader(const char* vertexPath, const char* tessCtrlPath,
               const char* tessEvalPath, const char* fragmentPath)
    : ID(0)
{
    std::string vertexCode;
    std::string tessCtrlCode;
    std::string tessEvalCode;
    std::string fragmentCode;

    std::ifstream vShaderFile;
    std::ifstream tcShaderFile;
    std::ifstream teShaderFile;
    std::ifstream fShaderFile;

    // Ensure ifstream objects can throw exceptions
    vShaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    tcShaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    teShaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    fShaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);

    try
    {
        // Open files
        vShaderFile.open(vertexPath);
        tcShaderFile.open(tessCtrlPath);
        teShaderFile.open(tessEvalPath);
        fShaderFile.open(fragmentPath);

        std::stringstream vShaderStream, tcShaderStream, teShaderStream, fShaderStream;
        vShaderStream  << vShaderFile.rdbuf();
        tcShaderStream << tcShaderFile.rdbuf();
        teShaderStream << teShaderFile.rdbuf();
        fShaderStream  << fShaderFile.rdbuf();

        vShaderFile.close();
        tcShaderFile.close();
        teShaderFile.close();
        fShaderFile.close();

        vertexCode     = vShaderStream.str();
        tessCtrlCode   = tcShaderStream.str();
        tessEvalCode   = teShaderStream.str();
        fragmentCode   = fShaderStream.str();
    }
    catch (std::ifstream::failure&)
    {
        std::cerr << "ERROR::SHADER::FILE_NOT_SUCCESSFULLY_READ (4-stage)\n";
        std::cerr << "Vertex Path:     " << vertexPath     << "\n";
        std::cerr << "TessCtrl Path:   " << tessCtrlPath   << "\n";
        std::cerr << "TessEval Path:   " << tessEvalPath   << "\n";
        std::cerr << "Fragment Path:   " << fragmentPath   << "\n";
        return;
    }

    const char* vShaderCode   = vertexCode.c_str();
    const char* tcShaderCode  = tessCtrlCode.c_str();
    const char* teShaderCode  = tessEvalCode.c_str();
    const char* fShaderCode   = fragmentCode.c_str();

    // ------------------------------------------------------------
    // Compile Vertex Shader
    // ------------------------------------------------------------
    unsigned int vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &vShaderCode, nullptr);
    glCompileShader(vertex);
    checkCompileErrors(vertex, "VERTEX");

    // ------------------------------------------------------------
    // Compile Tessellation Control Shader
    // ------------------------------------------------------------
    unsigned int tessControl = glCreateShader(GL_TESS_CONTROL_SHADER);
    glShaderSource(tessControl, 1, &tcShaderCode, nullptr);
    glCompileShader(tessControl);
    checkCompileErrors(tessControl, "TESS_CONTROL");

    // ------------------------------------------------------------
    // Compile Tessellation Evaluation Shader
    // ------------------------------------------------------------
    unsigned int tessEval = glCreateShader(GL_TESS_EVALUATION_SHADER);
    glShaderSource(tessEval, 1, &teShaderCode, nullptr);
    glCompileShader(tessEval);
    checkCompileErrors(tessEval, "TESS_EVALUATION");

    // ------------------------------------------------------------
    // Compile Fragment Shader
    // ------------------------------------------------------------
    unsigned int fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, nullptr);
    glCompileShader(fragment);
    checkCompileErrors(fragment, "FRAGMENT");

    // ------------------------------------------------------------
    // Shader Program (4 stages)
    // ------------------------------------------------------------
    ID = glCreateProgram();
    glAttachShader(ID, vertex);
    glAttachShader(ID, tessControl);
    glAttachShader(ID, tessEval);
    glAttachShader(ID, fragment);
    glLinkProgram(ID);
    checkCompileErrors(ID, "PROGRAM");

    glDeleteShader(vertex);
    glDeleteShader(tessControl);
    glDeleteShader(tessEval);
    glDeleteShader(fragment);
}

// ------------------------------------------------------------
// Constructor (from raw source strings)
// ------------------------------------------------------------
Shader::Shader(const std::string& vertexSource, const std::string& fragmentSource, bool fromString)
{
    (void)fromString;

    const char* vShaderCode = vertexSource.c_str();
    const char* fShaderCode = fragmentSource.c_str();

    unsigned int vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &vShaderCode, nullptr);
    glCompileShader(vertex);
    checkCompileErrors(vertex, "VERTEX");

    unsigned int fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, nullptr);
    glCompileShader(fragment);
    checkCompileErrors(fragment, "FRAGMENT");

    ID = glCreateProgram();
    glAttachShader(ID, vertex);
    glAttachShader(ID, fragment);
    glLinkProgram(ID);
    checkCompileErrors(ID, "PROGRAM");

    glDeleteShader(vertex);
    glDeleteShader(fragment);
}

// ------------------------------------------------------------
// Use Shader
// ------------------------------------------------------------
void Shader::use() const
{
    if (ID != 0) {
        glUseProgram(ID);
    }
}

// ------------------------------------------------------------
// Uniform Helpers (with location caching)
// ------------------------------------------------------------
GLint Shader::getUniformLocation(const std::string& name) const
{
    if (ID == 0) return -1;

    auto it = m_uniformLocations.find(name);
    if (it != m_uniformLocations.end())
        return it->second;

    GLint loc = glGetUniformLocation(ID, name.c_str());
    m_uniformLocations[name] = loc;
    return loc;
}

void Shader::setBool(const std::string& name, bool value) const
{
    glUniform1i(getUniformLocation(name), (int)value);
}

void Shader::setInt(const std::string& name, int value) const
{
    glUniform1i(getUniformLocation(name), value);
}

void Shader::setFloat(const std::string& name, float value) const
{
    glUniform1f(getUniformLocation(name), value);
}

void Shader::setVec3(const std::string& name, const glm::vec3& value) const
{
    glUniform3fv(getUniformLocation(name), 1, &value[0]);
}

void Shader::setMat4(const std::string& name, const glm::mat4& mat) const
{
    glUniformMatrix4fv(getUniformLocation(name), 1, GL_FALSE, &mat[0][0]);
}

// ------------------------------------------------------------
// Error Checking
// ------------------------------------------------------------
void Shader::checkCompileErrors(unsigned int shader, const std::string& type)
{
    int success;
    char infoLog[1024];

    if (type != "PROGRAM")
    {
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            glGetShaderInfoLog(shader, 1024, nullptr, infoLog);
            std::cerr << "ERROR::SHADER_COMPILATION_ERROR of type: " << type << "\n" << infoLog << "\n";
        }
    }
    else
    {
        glGetProgramiv(shader, GL_LINK_STATUS, &success);
        if (!success)
        {
            glGetProgramInfoLog(shader, 1024, nullptr, infoLog);
            std::cerr << "ERROR::PROGRAM_LINKING_ERROR\n" << infoLog << "\n";
        }
    }
}

