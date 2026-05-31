#include "placebo_egl_context.hpp"
#include "core/log/debug_log.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

// ---------------------------------------------------------------------------
// PlaceboEglContext implementation
// ---------------------------------------------------------------------------

bool PlaceboEglContext::create() {
    // Use EGL_MESA_platform_surfaceless for a headless context (no window needed).
    auto eglGetPlatformDisplayEXT =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));

    if (eglGetPlatformDisplayEXT) {
        m_display = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
                                             EGL_DEFAULT_DISPLAY, nullptr);
    }
    if (m_display == EGL_NO_DISPLAY) {
        // Fallback: default display (may work on some drivers)
        m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (m_display == EGL_NO_DISPLAY) {
        APP_DEBUG_LOG("[PlaceboEglContext] eglGetDisplay failed");
        return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(m_display, &major, &minor)) {
        APP_DEBUG_LOG("[PlaceboEglContext] eglInitialize failed: 0x{:x}", eglGetError());
        m_display = EGL_NO_DISPLAY;
        return false;
    }
    APP_DEBUG_LOG("[PlaceboEglContext] EGL {}.{} initialized", major, minor);

    // Bind OpenGL (not OpenGL ES) as the API.
    if (!eglBindAPI(EGL_OPENGL_API)) {
        APP_DEBUG_LOG("[PlaceboEglContext] eglBindAPI(EGL_OPENGL_API) failed");
        eglTerminate(m_display);
        m_display = EGL_NO_DISPLAY;
        return false;
    }

    // Choose a minimal config — surfaceless contexts don't need surface attrs.
    static const EGLint config_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint num_configs = 0;
    if (!eglChooseConfig(m_display, config_attribs, &config, 1, &num_configs)
        || num_configs < 1) {
        APP_DEBUG_LOG("[PlaceboEglContext] eglChooseConfig failed");
        eglTerminate(m_display);
        m_display = EGL_NO_DISPLAY;
        return false;
    }

    // Request at least OpenGL 3.3 core for GL_EXT_memory_object support.
    static const EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    m_ctx = eglCreateContext(m_display, config, EGL_NO_CONTEXT, ctx_attribs);
    if (m_ctx == EGL_NO_CONTEXT) {
        APP_DEBUG_LOG("[PlaceboEglContext] eglCreateContext failed: 0x{:x}", eglGetError());
        eglTerminate(m_display);
        m_display = EGL_NO_DISPLAY;
        return false;
    }

    APP_DEBUG_LOG("[PlaceboEglContext] context created");
    return true;
}

void PlaceboEglContext::destroy() {
    if (m_ctx != EGL_NO_CONTEXT) {
        // Make sure the context is not current before destroying.
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(m_display, m_ctx);
        m_ctx = EGL_NO_CONTEXT;
    }
    if (m_display != EGL_NO_DISPLAY) {
        eglTerminate(m_display);
        m_display = EGL_NO_DISPLAY;
    }
}

bool PlaceboEglContext::make_current() {
    if (m_ctx == EGL_NO_CONTEXT)
        return false;
    const bool ok = eglMakeCurrent(m_display,
                                   EGL_NO_SURFACE, EGL_NO_SURFACE,
                                   m_ctx) == EGL_TRUE;
    if (!ok)
        APP_DEBUG_LOG("[PlaceboEglContext] make_current failed: 0x{:x}", eglGetError());
    return ok;
}

bool PlaceboEglContext::release() {
    if (m_display == EGL_NO_DISPLAY)
        return true;
    return eglMakeCurrent(m_display,
                          EGL_NO_SURFACE, EGL_NO_SURFACE,
                          EGL_NO_CONTEXT) == EGL_TRUE;
}

void *PlaceboEglContext::get_proc_address(void * /*ctx*/, const char *name) {
    return reinterpret_cast<void *>(eglGetProcAddress(name));
}
