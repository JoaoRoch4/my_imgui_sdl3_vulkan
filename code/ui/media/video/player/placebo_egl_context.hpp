#pragma once

#include "pch.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <mpv/render_gl.h>

// ---------------------------------------------------------------------------
// PlaceboEglContext
//
// Manages a single headless EGL context used as the OpenGL render backend
// for one mpv instance.
//
// Lifecycle:
//   create() → use context (make_current / release) → destroy()
//
// Thread safety:
//   ALL calls (create, make_current, release, destroy) MUST happen on the
//   same thread — the main render thread.  This matches the constraint that
//   mpv_render_context_render() must be called from the thread that owns the
//   GL context, and avoids black-screen / driver bugs caused by GL context
//   migration across OS threads.
// ---------------------------------------------------------------------------
class PlaceboEglContext {
public:
    PlaceboEglContext() = default;
    ~PlaceboEglContext() { destroy(); }

    PlaceboEglContext(const PlaceboEglContext &) = delete;
    PlaceboEglContext &operator=(const PlaceboEglContext &) = delete;

    // Create a surfaceless headless EGL context.
    // Returns true on success.
    bool create();
    void destroy();

    // Make the context current on the calling thread (surfaceless).
    bool make_current();
    bool release();

    [[nodiscard]] bool valid() const noexcept { return m_ctx != EGL_NO_CONTEXT; }
    [[nodiscard]] EGLDisplay display() const noexcept { return m_display; }
    [[nodiscard]] EGLContext context() const noexcept { return m_ctx; }

    // Returns a proc address for mpv's OpenGL init params.
    static void *get_proc_address(void */*ctx*/, const char *name);

private:
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLContext m_ctx     = EGL_NO_CONTEXT;
};
