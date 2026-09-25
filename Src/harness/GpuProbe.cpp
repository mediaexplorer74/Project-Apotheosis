// GpuProbe.cpp — see GpuProbe.h. Uses the legacy Microsoft ANGLE.WindowsStore (ms-master) PropertySet pattern:
// the SwapChainPanel is handed to eglCreateWindowSurface via a PropertySet (EGLNativeWindowTypeProperty);
// the D3D11 backend initializes at FL9_3 (MAX_VERSION 9.3 = D3D feature level, covering the lower bound of
// old devices like the 1020). CompileAsWinRT=true (PropertySet/SwapChainPanel are used), but no PCH (standalone).
#include <windows.h>
#include <inspectable.h>
#include <agile.h>            // Platform::Agile<>
#include <cstdio>
#include <string>
#include <thread>
#include <fstream>
// This ms-master ANGLE's gl2.h puts core GL function prototypes under GL_GLEXT_PROTOTYPES (otherwise C3861: glClear not found).
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include "GpuProbe.h"

using namespace Windows::UI::Xaml::Controls;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;

static std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static GLuint CompileShader(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}

void Harness::RunGpuProbe(SwapChainPanel^ panel, Platform::String^ localStateDir)
{
    Platform::Agile<SwapChainPanel^> agile(panel);
    std::wstring dir = localStateDir ? std::wstring(localStateDir->Data()) : L"";

    std::thread([agile, dir]() {
        std::string log;
        char buf[160];
        auto step = [&](const char* m) { log += m; log += "\n"; };
        auto write = [&]() {
            if (dir.empty()) return;
            std::ofstream f(Narrow(dir) + "\\gpuprobe.txt", std::ios::binary | std::ios::trunc);
            if (f) f.write(log.data(), log.size());
        };

        step("=== GPU probe: ANGLE(D3D11 FL9_3) + SwapChainPanel + GL ES2 triangle ===");

        auto getPlatDisp = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (!getPlatDisp) { step("FAIL: eglGetPlatformDisplayEXT not found (libEGL not loaded?)"); write(); return; }

        // MAX_VERSION 9.3 = request D3D11 feature level 9_3 (legacy ms-master ANGLE semantics), verifying the old-device lower bound.
        const EGLint dattr[] = {
            EGL_PLATFORM_ANGLE_TYPE_ANGLE,              EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
            EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE, 9,
            EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE, 3,
            EGL_NONE
        };
        EGLDisplay dpy = getPlatDisp(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, dattr);
        if (dpy == EGL_NO_DISPLAY) { step("FAIL: eglGetPlatformDisplayEXT -> NO_DISPLAY"); write(); return; }
        if (!eglInitialize(dpy, nullptr, nullptr)) { sprintf_s(buf, "FAIL: eglInitialize err=0x%x", eglGetError()); step(buf); write(); return; }
        step("OK: eglInitialize(D3D11 FL9_3)");

        const EGLint cattr[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_DEPTH_SIZE, 16, EGL_STENCIL_SIZE, 8,
            EGL_NONE
        };
        EGLConfig cfg; EGLint nc = 0;
        if (!eglChooseConfig(dpy, cattr, &cfg, 1, &nc) || nc < 1) { step("FAIL: eglChooseConfig (no ES2/RGBA8/D16S8 config)"); write(); return; }
        step("OK: eglChooseConfig");

        SwapChainPanel^ p = agile.Get();
        if (!p) { step("FAIL: SwapChainPanel agile is null"); write(); return; }
        // ms-master ANGLE.WindowsStore: native window = PropertySet (holds the SwapChainPanel + fixed render size).
        PropertySet^ props = ref new PropertySet();
        props->Insert(L"EGLNativeWindowTypeProperty", p);
        props->Insert(L"EGLRenderSurfaceSizeProperty", PropertyValue::CreateSize(Size(720, 1080)));
        EGLNativeWindowType win = reinterpret_cast<EGLNativeWindowType>(reinterpret_cast<IInspectable*>(props));
        EGLSurface surf = eglCreateWindowSurface(dpy, cfg, win, nullptr);
        if (surf == EGL_NO_SURFACE) { sprintf_s(buf, "FAIL: eglCreateWindowSurface err=0x%x", eglGetError()); step(buf); write(); return; }
        step("OK: eglCreateWindowSurface(SwapChainPanel)");

        const EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
        if (ctx == EGL_NO_CONTEXT) { sprintf_s(buf, "FAIL: eglCreateContext err=0x%x", eglGetError()); step(buf); write(); return; }
        if (!eglMakeCurrent(dpy, surf, surf, ctx)) { sprintf_s(buf, "FAIL: eglMakeCurrent err=0x%x", eglGetError()); step(buf); write(); return; }
        step("OK: GL ES2 context current");

        const char* vendor   = (const char*)glGetString(GL_VENDOR);
        const char* renderer = (const char*)glGetString(GL_RENDERER);
        const char* version  = (const char*)glGetString(GL_VERSION);
        sprintf_s(buf, "GL_VENDOR=%s",   vendor   ? vendor   : "?"); step(buf);
        sprintf_s(buf, "GL_RENDERER=%s", renderer ? renderer : "?"); step(buf);   // D3D11 adapter name: proves real hardware
        sprintf_s(buf, "GL_VERSION=%s",  version  ? version  : "?"); step(buf);

        GLuint vs = CompileShader(GL_VERTEX_SHADER,   "attribute vec2 p;void main(){gl_Position=vec4(p,0.0,1.0);}");
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, "precision mediump float;uniform float t;void main(){gl_FragColor=vec4(1.0,0.55+0.45*t,0.10,1.0);}");
        GLuint prog = glCreateProgram();
        glAttachShader(prog, vs); glAttachShader(prog, fs);
        glBindAttribLocation(prog, 0, "p");
        glLinkProgram(prog);
        GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked);
        if (!linked) { step("FAIL: shader program link failed"); write(); return; }
        glUseProgram(prog);
        GLint tloc = glGetUniformLocation(prog, "t");

        const GLfloat tri[] = { 0.0f, 0.8f,  -0.8f, -0.8f,  0.8f, -0.8f };
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
        glViewport(0, 0, 720, 1080);

        // ~48 frames of animation (triangle fading orange -> yellow), proving sustained GPU rendering +
        // swapchain Present work.
        for (int i = 0; i < 48; ++i) {
            float t = (float)i / 48.0f;
            glClearColor(0.05f, 0.12f, 0.22f, 1.0f);   // dark-blue background
            glClear(GL_COLOR_BUFFER_BIT);
            glUniform1f(tloc, t);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            eglSwapBuffers(dpy, surf);
        }
        GLenum gl = glGetError();
        sprintf_s(buf, "SUCCESS: drew triangle for 48 frames, glError=0x%x", gl); step(buf);
        step("* If you see a dark-blue background + orange/yellow triangle on the device = GPU pipeline works in the App Container!");
        write();
        // Keep the context/last frame (the triangle stays on screen). Probe-process exit is managed by the app lifecycle.
    }).detach();
}
