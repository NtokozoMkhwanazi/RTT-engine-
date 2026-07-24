#include "viewport_framebuffer.h"

#include <iostream>

namespace Editor {

ViewportFramebuffer::ViewportFramebuffer(int width, int height) {
    initialize(width, height);
}

ViewportFramebuffer::~ViewportFramebuffer() {
    cleanup();
}

ViewportFramebuffer::ViewportFramebuffer(ViewportFramebuffer&& other) noexcept
    : fbo_(other.fbo_),
      colorTex_(other.colorTex_),
      rbo_(other.rbo_),
      width_(other.width_),
      height_(other.height_) {
    other.fbo_ = 0;
    other.colorTex_ = 0;
    other.rbo_ = 0;
    other.width_ = 0;
    other.height_ = 0;
}

ViewportFramebuffer& ViewportFramebuffer::operator=(ViewportFramebuffer&& other) noexcept {
    if (this != &other) {
        cleanup();
        fbo_ = other.fbo_;
        colorTex_ = other.colorTex_;
        rbo_ = other.rbo_;
        width_ = other.width_;
        height_ = other.height_;

        other.fbo_ = 0;
        other.colorTex_ = 0;
        other.rbo_ = 0;
        other.width_ = 0;
        other.height_ = 0;
    }
    return *this;
}

bool ViewportFramebuffer::initialize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    cleanup();
    width_ = width;
    height_ = height;

    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);

    glGenTextures(1, &colorTex_);
    glBindTexture(GL_TEXTURE_2D, colorTex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width_, height_, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex_, 0);

    glGenRenderbuffers(1, &rbo_);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width_, height_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo_);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "ERROR: Framebuffer incomplete!\n";
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

void ViewportFramebuffer::resize(int width, int height) {
    if (width <= 0 || height <= 0 || (width == width_ && height == height_)) return;
    initialize(width, height);
}

void ViewportFramebuffer::cleanup() noexcept {
    if (fbo_) glDeleteFramebuffers(1, &fbo_);
    if (colorTex_) glDeleteTextures(1, &colorTex_);
    if (rbo_) glDeleteRenderbuffers(1, &rbo_);
    fbo_ = 0;
    colorTex_ = 0;
    rbo_ = 0;
    width_ = 0;
    height_ = 0;
}

void ViewportFramebuffer::bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
}

void ViewportFramebuffer::unbind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool ViewportFramebuffer::isComplete() const {
    if (fbo_ == 0) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    const bool complete = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return complete;
}

} // namespace Editor
