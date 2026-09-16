#ifndef EDITOR_VIEWPORT_FRAMEBUFFER_H
#define EDITOR_VIEWPORT_FRAMEBUFFER_H

#include <glad/glad.h>

namespace Editor {

class ViewportFramebuffer {
public:
    ViewportFramebuffer() = default;
    explicit ViewportFramebuffer(int width, int height);
    ~ViewportFramebuffer();

    ViewportFramebuffer(const ViewportFramebuffer&) = delete;
    ViewportFramebuffer& operator=(const ViewportFramebuffer&) = delete;

    ViewportFramebuffer(ViewportFramebuffer&& other) noexcept;
    ViewportFramebuffer& operator=(ViewportFramebuffer&& other) noexcept;

    bool initialize(int width, int height);
    void resize(int width, int height);
    void cleanup() noexcept;

    void bind() const;
    void unbind() const;

    bool isComplete() const;
    bool isInitialized() const noexcept { return fbo_ != 0; }

    GLuint fbo() const noexcept { return fbo_; }
    GLuint colorTexture() const noexcept { return colorTex_; }
    GLuint rbo() const noexcept { return rbo_; }
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

private:
    GLuint fbo_ = 0;
    GLuint colorTex_ = 0;
    GLuint rbo_ = 0;
    int width_ = 0;
    int height_ = 0;
};

} // namespace Editor

#endif
