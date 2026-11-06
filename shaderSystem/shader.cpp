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

// ------------------------------------------------------------
// Constructor (4 stages: vertex + tessellation control +
// tessellation evaluation + fragment)
// ------------------------------------------------------------
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

    vShaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    tcShaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    teShaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);
    fShaderFile.exceptions(std::ifstream::failbit | std::ifstream::badbit);

    try
    {
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

        vertexCode   = vShaderStream.str();
        tessCtrlCode = tcShaderStream.str();
        tessEvalCode = teShaderStream.str();
        fragmentCode = fShaderStream.str();
    }
    catch (std::ifstream::failure&)
    {
        std::cerr << "ERROR::SHADER::FILE_NOT_SUCCESSFULLY_READ\n";
        std::cerr << "Vertex Path:   " << vertexPath << "\n";
        std::cerr << "TessCtrl Path: " << tessCtrlPath << "\n";
        std::cerr << "TessEval Path: " << tessEvalPath << "\n";
        std::cerr << "Fragment Path: " << fragmentPath << "\n";
        return;
    }

    const char* vShaderCode   = vertexCode.c_str();
    const char* tcShaderCode  = tessCtrlCode.c_str();
    const char* teShaderCode  = tessEvalCode.c_str();
    const char* fShaderCode   = fragmentCode.c_str();

    unsigned int vertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex, 1, &vShaderCode, nullptr);
    glCompileShader(vertex);
    checkCompileErrors(vertex, "VERTEX");

    unsigned int tessCtrl = glCreateShader(GL_TESS_CONTROL_SHADER);
    glShaderSource(tessCtrl, 1, &tcShaderCode, nullptr);
    glCompileShader(tessCtrl);
    checkCompileErrors(tessCtrl, "TESS_CONTROL");

    unsigned int tessEval = glCreateShader(GL_TESS_EVALUATION_SHADER);
    glShaderSource(tessEval, 1, &teShaderCode, nullptr);
    glCompileShader(tessEval);
    checkCompileErrors(tessEval, "TESS_EVALUATION");

    unsigned int fragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment, 1, &fShaderCode, nullptr);
    glCompileShader(fragment);
    checkCompileErrors(fragment, "FRAGMENT");

    ID = glCreateProgram();
    glAttachShader(ID, vertex);
    glAttachShader(ID, tessCtrl);
    glAttachShader(ID, tessEval);
    glAttachShader(ID, fragment);
    glLinkProgram(ID);
    checkCompileErrors(ID, "PROGRAM");

    glDeleteShader(vertex);
    glDeleteShader(tessCtrl);
    glDeleteShader(tessEval);
    glDeleteShader(fragment);
}

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
    // Don't glUseProgram() a program that failed to link. glUseProgram on a
    // non-linked program is a no-op that emits GL_INVALID_OPERATION, so a
    // broken shader would silently draw nothing every frame (the exact cause
    // of "terrain invisible despite loaded"). Gating on m_linked makes a
    // link failure explicit: the caller gets no shader bound rather than a
    // silently-invalid draw. (Callers should also check isLinked() before use.)
    if (ID != 0 && m_linked) {
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
        m_linked = (success != 0);
        if (!success)
        {
            glGetProgramInfoLog(shader, 1024, nullptr, infoLog);
            std::cerr << "ERROR::PROGRAM_LINKING_ERROR\n" << infoLog << "\n";
        }
    }
}

