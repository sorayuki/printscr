#include "PreviewModule.h"
#include "GpuFrame.h"
#include "Logger.h"
#include "SystemInfo.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

class PreviewWindowImpl : public PreviewWindow {
public:
    PreviewWindowImpl(EGLDisplay display, EGLSurface dummySurface, EGLContext rootContext)
        : m_display(display), m_dummySurface(dummySurface), m_rootContext(rootContext) {}

    ~PreviewWindowImpl() { CleanupGL(); }

    SelectionRect Show(std::shared_ptr<GpuFrame> gpuFrame) override {
        LOG("PreviewWindow::Show called.");
        m_gpuFrame = gpuFrame;
        m_hdrInfo = SystemInfo::GetPrimaryDisplayHdrInfo();
        LOG("HDR Info: SDR White Level=" + std::to_string(m_hdrInfo.sdrWhiteLevel));

        LOG("Creating Win32 window...");
        if (!CreateWin32Window()) {
            LOG("Failed to create Win32 window.");
            return {};
        }
        LOG("Window created.");

        LOG("Initializing EGL Surface...");
        if (!InitEGLSurface()) {
            LOG("Failed to initialize EGL surface.");
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
            return {};
        }

        if (m_program == 0) {
            LOG("Initializing GL (first time)...");
            if (!InitGL()) {
                LOG("Failed to initialize GL.");
                CleanupSurface();
                DestroyWindow(m_hwnd);
                m_hwnd = nullptr;
                return {};
            }
            LOG("GL initialized.");
        } else {
            // Re-bind texture properly inside InitGL/Render if needed, we just update m_texture here
            m_texture = m_gpuFrame->GetTextureId();
        }

        m_running = true;
        m_selectionConfirmed = false;
        m_selection = {0, 0, 0, 0};
        m_needsRender = true;
        m_currentSwapInterval = -1;

        // Cache cursors
        m_cursorCross = LoadCursorW(nullptr, (LPCWSTR)IDC_CROSS);
        m_cursorArrow = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        m_cursorSizeNWSE = LoadCursorW(nullptr, (LPCWSTR)IDC_SIZENWSE);
        m_cursorSizeNESW = LoadCursorW(nullptr, (LPCWSTR)IDC_SIZENESW);
        m_cursorSizeNS = LoadCursorW(nullptr, (LPCWSTR)IDC_SIZENS);
        m_cursorSizeWE = LoadCursorW(nullptr, (LPCWSTR)IDC_SIZEWE);
        m_cursorSizeAll = LoadCursorW(nullptr, (LPCWSTR)IDC_SIZEALL);

        LOG("Entering message loop...");

        MSG msg;
        while (m_running) {
            if (!m_needsRender) {
                BOOL waitResult = GetMessage(&msg, nullptr, 0, 0);
                if (waitResult <= 0) {
                    m_running = false;
                    break;
                }

                if (msg.message == WM_QUIT) {
                    // Because we might have a lingering WM_QUIT from another part, we just consume it.
                    // If it was meant for us, m_running will become false eventually anyway.
                    // Wait, if it's meant for the app, we should probably repost it. Actually,
                    // we remove WM_QUIT posting from WM_DESTROY, so WM_QUIT should never happen
                    // unless the app is terminating.
                    m_running = false;
                } else {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            }

            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    m_running = false;
                } else {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            }

            if (!m_running)
                break;

            UpdateSwapInterval();

            if (!m_needsRender)
                continue;

            Render();
            if (!eglSwapBuffers(m_display, m_surface)) {
                LOG("eglSwapBuffers failed.");
                break;
            }

            m_needsRender = false;
        }

        LOG("Exiting message loop. Cleaning up surface...");
        CleanupSurface();
        if (m_hwnd) {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }

        // We MUST pump the queue for WM_DESTROY and WM_NCDESTROY otherwise DestroyWindow won't finish its message sending
        // Actually DestroyWindow is synchronous for messages, but we should handle pending ones if any.
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        LOG("Surface cleanup done. Show returning.");
        return m_selectionConfirmed ? m_selection : SelectionRect{0, 0, 0, 0};
    }

private:
    bool CreateWin32Window();
    bool InitEGLSurface();
    bool InitGL();
    void Render();
    void CleanupSurface();
    void CleanupGL();
    void UpdateSwapInterval();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    EGLDisplay m_display = EGL_NO_DISPLAY;
    EGLSurface m_dummySurface = EGL_NO_SURFACE;
    EGLContext m_rootContext = EGL_NO_CONTEXT;
    
    EGLConfig m_config = nullptr;
    EGLSurface m_surface = EGL_NO_SURFACE;
    EGLContext m_context = EGL_NO_CONTEXT;

    std::shared_ptr<GpuFrame> m_gpuFrame;
    DisplayHdrInfo m_hdrInfo;
    bool m_running = false;
    bool m_selectionConfirmed = false;

    SelectionRect m_selection = {0, 0, 0, 0};
    bool m_isDragging = false;
    int m_dragMode = 0; // 0: new rect, 1-4: corners, 5-8: edges, 9: move

    GLuint m_program = 0;
    GLuint m_texture = 0;
    GLuint m_vbo = 0;

    int m_windowWidth = 0;
    int m_windowHeight = 0;

    int m_lastMouseX = 0;
    int m_lastMouseY = 0;
    bool m_needsRender = false;
    EGLint m_currentSwapInterval = -1;

    HCURSOR m_cursorCross = nullptr;
    HCURSOR m_cursorArrow = nullptr;
    HCURSOR m_cursorSizeNWSE = nullptr;
    HCURSOR m_cursorSizeNESW = nullptr;
    HCURSOR m_cursorSizeNS = nullptr;
    HCURSOR m_cursorSizeWE = nullptr;
    HCURSOR m_cursorSizeAll = nullptr;

    int GetDragMode(int x, int y) {
        if (!m_selection.IsValid())
            return 0;

        int l = m_selection.Left();
        int t = m_selection.Top();
        int r = m_selection.Right();
        int b = m_selection.Bottom();

        UINT dpi = GetDpiForWindow(m_hwnd);
        if (dpi == 0)
            dpi = 96;
        const int tol = (3 * dpi) / 96;

        bool hitL = std::abs(x - l) <= tol;
        bool hitR = std::abs(x - r) <= tol;
        bool hitT = std::abs(y - t) <= tol;
        bool hitB = std::abs(y - b) <= tol;

        if (hitL && hitT)
            return 1; // TL
        if (hitR && hitT)
            return 2; // TR
        if (hitR && hitB)
            return 3; // BR
        if (hitL && hitB)
            return 4; // BL
        if (hitT && x >= l && x <= r)
            return 5; // T
        if (hitR && y >= t && y <= b)
            return 6; // R
        if (hitB && x >= l && x <= r)
            return 7; // B
        if (hitL && y >= t && y <= b)
            return 8; // L
        if (x > l && x < r && y > t && y < b)
            return 9; // Move

        return 0; // New
    }
};

std::unique_ptr<PreviewWindow> PreviewWindow::Create(EGLDisplay display, EGLSurface dummySurface, EGLContext context) {
    return std::make_unique<PreviewWindowImpl>(display, dummySurface, context);
}

LRESULT CALLBACK PreviewWindowImpl::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    PreviewWindowImpl *self = (PreviewWindowImpl *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    if (!self && msg != WM_CREATE) {
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case WM_CREATE: {
            LOG("WM_CREATE received.");
            CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return 0;
        }
        case WM_CLOSE:
            LOG("WM_CLOSE received.");
            self->m_running = false;
            return 0;
        case WM_KEYDOWN:
            // LOG("WM_KEYDOWN: " + std::to_string(wParam));
            if (wParam == VK_ESCAPE) {
                self->m_selectionConfirmed = false;
                self->m_running = false;
            } else if (wParam == VK_RETURN) {
                self->m_selectionConfirmed = true;
                self->m_running = false;
            }
            return 0;
        case WM_LBUTTONDOWN: {
            // LOG("WM_LBUTTONDOWN");
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            self->m_lastMouseX = x;
            self->m_lastMouseY = y;
            self->m_isDragging = true;
            self->m_dragMode = self->GetDragMode(x, y);

            if (self->m_dragMode == 0) {
                self->m_selection.x1 = x;
                self->m_selection.y1 = y;
                self->m_selection.x2 = x;
                self->m_selection.y2 = y;
            } else {
                // Normalize current selection for easier drag logic
                int l = self->m_selection.Left();
                int t = self->m_selection.Top();
                int r = self->m_selection.Right();
                int b = self->m_selection.Bottom();
                self->m_selection.x1 = l;
                self->m_selection.y1 = t;
                self->m_selection.x2 = r;
                self->m_selection.y2 = b;
            }
            self->m_needsRender = true;
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            if (self->m_isDragging) {
                switch (self->m_dragMode) {
                    case 0: // New
                        self->m_selection.x2 = x;
                        self->m_selection.y2 = y;
                        break;
                    case 1: // TL
                        self->m_selection.x1 = x;
                        self->m_selection.y1 = y;
                        break;
                    case 2: // TR
                        self->m_selection.x2 = x;
                        self->m_selection.y1 = y;
                        break;
                    case 3: // BR
                        self->m_selection.x2 = x;
                        self->m_selection.y2 = y;
                        break;
                    case 4: // BL
                        self->m_selection.x1 = x;
                        self->m_selection.y2 = y;
                        break;
                    case 5: // T
                        self->m_selection.y1 = y;
                        break;
                    case 6: // R
                        self->m_selection.x2 = x;
                        break;
                    case 7: // B
                        self->m_selection.y2 = y;
                        break;
                    case 8: // L
                        self->m_selection.x1 = x;
                        break;
                    case 9: // Move
                    {
                        int dx = x - self->m_lastMouseX;
                        int dy = y - self->m_lastMouseY;
                        self->m_selection.x1 += dx;
                        self->m_selection.x2 += dx;
                        self->m_selection.y1 += dy;
                        self->m_selection.y2 += dy;
                        break;
                    }
                }
                self->m_lastMouseX = x;
                self->m_lastMouseY = y;
                self->m_needsRender = true;
            } else {
                // Update cursor feedback
                int mode = self->GetDragMode(x, y);
                HCURSOR cursor = self->m_cursorCross;
                switch (mode) {
                    case 1:
                    case 3:
                        cursor = self->m_cursorSizeNWSE;
                        break;
                    case 2:
                    case 4:
                        cursor = self->m_cursorSizeNESW;
                        break;
                    case 5:
                    case 7:
                        cursor = self->m_cursorSizeNS;
                        break;
                    case 6:
                    case 8:
                        cursor = self->m_cursorSizeWE;
                        break;
                    case 9:
                        cursor = self->m_cursorSizeAll;
                        break;
                    default:
                        cursor = self->m_cursorCross;
                        break;
                }
                SetCursor(cursor);
            }
            return 0;
        }
        case WM_SETCURSOR: {
            // Prevent DefWindowProc from resetting our cursor
            if (LOWORD(lParam) == HTCLIENT) {
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDBLCLK: {
            LOG("WM_LBUTTONDBLCLK");
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            if (self->m_selection.IsValid()) {
                int l = self->m_selection.Left();
                int t = self->m_selection.Top();
                int r = self->m_selection.Right();
                int b = self->m_selection.Bottom();

                if (x > l && x < r && y > t && y < b) {
                    self->m_selectionConfirmed = true;
                    self->m_running = false;
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            // LOG("WM_LBUTTONUP");
            self->m_isDragging = false;
            return 0;
        }
        case WM_RBUTTONUP: {
            LOG("WM_RBUTTONUP");
            self->m_selectionConfirmed = false;
            self->m_running = false;
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            LOG("WM_DESTROY received.");
            // Do NOT PostQuitMessage(0); here! That will poison the next Show()
            self->m_running = false;
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool PreviewWindowImpl::CreateWin32Window() {
    WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
    if (!GetClassInfoExW(GetModuleHandle(nullptr), L"PrintscrPreview", &wc)) {
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"PrintscrPreview";
        wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
        wc.style = CS_DBLCLKS;
        RegisterClassExW(&wc);
    }

    m_windowWidth = GetSystemMetrics(SM_CXSCREEN);
    m_windowHeight = GetSystemMetrics(SM_CYSCREEN);

    m_hwnd = CreateWindowExW(WS_EX_TOPMOST, L"PrintscrPreview", L"Preview", WS_POPUP | WS_VISIBLE, 0, 0, m_windowWidth,
                             m_windowHeight, nullptr, nullptr, GetModuleHandle(nullptr), this);

    return m_hwnd != nullptr;
}

bool PreviewWindowImpl::InitEGLSurface() {
    if (m_display == EGL_NO_DISPLAY)
        return false;

    if (m_context == EGL_NO_CONTEXT) {
        EGLint configAttribs[] = {EGL_RED_SIZE,
                                  16,
                                  EGL_GREEN_SIZE,
                                  16,
                                  EGL_BLUE_SIZE,
                                  16,
                                  EGL_ALPHA_SIZE,
                                  16,
                                  EGL_DEPTH_SIZE,
                                  24,
                                  EGL_STENCIL_SIZE,
                                  8,
                                  EGL_RENDERABLE_TYPE,
                                  EGL_OPENGL_ES3_BIT,
                                  EGL_SURFACE_TYPE,
                                  EGL_WINDOW_BIT,
                                  EGL_COLOR_COMPONENT_TYPE_EXT,
                                  EGL_COLOR_COMPONENT_TYPE_FLOAT_EXT,
                                  EGL_NONE};

        EGLint numConfigs;
        if (!eglChooseConfig(m_display, configAttribs, &m_config, 1, &numConfigs) || numConfigs == 0) {
            return false;
        }

        EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        m_context = eglCreateContext(m_display, m_config, m_rootContext, contextAttribs);
        if (m_context == EGL_NO_CONTEXT)
            return false;
    }

    EGLint surfaceAttribs[] = {EGL_DIRECT_COMPOSITION_ANGLE, TRUE, EGL_NONE};
    m_surface = eglCreateWindowSurface(m_display, m_config, m_hwnd, surfaceAttribs);
    if (m_surface == EGL_NO_SURFACE)
        return false;

    if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context))
        return false;

    return true;
}

const char *vShaderSource = R"(#version 300 es
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_texCoord;
out vec2 v_texCoord;
void main() {
    gl_Position = vec4(a_position, 0.0, 1.0);
    v_texCoord = a_texCoord;
}
)";

const char *fShaderSource = R"(#version 300 es
precision highp float;
uniform sampler2D u_texture;
uniform vec4 u_selection; // x1, y1, x2, y2 in normalized coords (0 to 1, top-down)
uniform float u_sdrWhitePointRatio; // sdrWhitePointNits / 80.0
uniform bool u_hasSelection;
uniform vec2 u_resolution;
uniform float u_borderWidth;
in vec2 v_texCoord;
out vec4 o_color;

void main() {
    vec4 color = texture(u_texture, v_texCoord);
    
    if (!u_hasSelection) {
        o_color = color;
        return;
    }

    float left = min(u_selection.x, u_selection.z);
    float right = max(u_selection.x, u_selection.z);
    float top = min(u_selection.y, u_selection.w);
    float bottom = max(u_selection.y, u_selection.w);

    bool inside = v_texCoord.x >= left && v_texCoord.x <= right &&
                  v_texCoord.y >= top && v_texCoord.y <= bottom;

    // Border thickness
    float thicknessX = u_borderWidth / u_resolution.x;
    float thicknessY = u_borderWidth / u_resolution.y;

    bool nearLeft = abs(v_texCoord.x - left) < thicknessX;
    bool nearRight = abs(v_texCoord.x - right) < thicknessX;
    bool nearTop = abs(v_texCoord.y - top) < thicknessY;
    bool nearBottom = abs(v_texCoord.y - bottom) < thicknessY;
    
    bool inYRange = v_texCoord.y >= top - thicknessY && v_texCoord.y <= bottom + thicknessY;
    bool inXRange = v_texCoord.x >= left - thicknessX && v_texCoord.x <= right + thicknessX;

    bool onBorder = false;
    if ((nearLeft || nearRight) && inYRange) onBorder = true;
    if ((nearTop || nearBottom) && inXRange) onBorder = true;

    if (onBorder) {
        o_color = vec4(1.0, 0.0, 0.0, 1.0); // Red
    } else if (inside) {
        o_color = color;
    } else {
        // Compress to SDR range: clamp to sdrWhitePointRatio
        vec3 clamped = min(color.rgb, vec3(u_sdrWhitePointRatio));
        // Reduce 80% brightness (20% remaining)
        o_color = vec4(clamped * 0.2, color.a);
    }
}
)";

bool PreviewWindowImpl::InitGL() {
    auto createShader = [](GLenum type, const char *source) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint success;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetShaderInfoLog(shader, 512, nullptr, infoLog);
            std::cerr << "Shader compilation error: " << infoLog << std::endl;
        }
        return shader;
    };

    GLuint vShader = createShader(GL_VERTEX_SHADER, vShaderSource);
    GLuint fShader = createShader(GL_FRAGMENT_SHADER, fShaderSource);
    m_program = glCreateProgram();
    glAttachShader(m_program, vShader);
    glAttachShader(m_program, fShader);
    glLinkProgram(m_program);
    glDeleteShader(vShader);
    glDeleteShader(fShader);

    // Quad data
    float vertices[] = {
        -1.0f, 1.0f, 0.0f, 0.0f, -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, -1.0f, 1.0f, 1.0f,
    };
    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // Texture
    m_texture = m_gpuFrame->GetTextureId();

    return true;
}

void PreviewWindowImpl::UpdateSwapInterval() {
    EGLint desiredSwapInterval = m_isDragging ? 0 : 1;
    if (desiredSwapInterval == m_currentSwapInterval) {
        return;
    }

    if (eglSwapInterval(m_display, desiredSwapInterval)) {
        m_currentSwapInterval = desiredSwapInterval;
    }
}

void PreviewWindowImpl::Render() {
    glViewport(0, 0, m_windowWidth, m_windowHeight);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(m_program);

    // Uniforms
    GLint locSelection = glGetUniformLocation(m_program, "u_selection");
    float x1 = (float)m_selection.x1 / m_windowWidth;
    float y1 = (float)m_selection.y1 / m_windowHeight;
    float x2 = (float)m_selection.x2 / m_windowWidth;
    float y2 = (float)m_selection.y2 / m_windowHeight;
    glUniform4f(locSelection, x1, y1, x2, y2);

    GLint locSdr = glGetUniformLocation(m_program, "u_sdrWhitePointRatio");
    glUniform1f(locSdr, m_hdrInfo.sdrWhiteLevel / 80.0f);

    GLint locHasSelection = glGetUniformLocation(m_program, "u_hasSelection");
    glUniform1i(locHasSelection, m_selection.IsValid() ? 1 : 0);

    GLint locResolution = glGetUniformLocation(m_program, "u_resolution");
    glUniform2f(locResolution, (float)m_windowWidth, (float)m_windowHeight);

    UINT dpi = GetDpiForWindow(m_hwnd);
    if (dpi == 0)
        dpi = 96;
    float borderWidth = std::round((float)dpi / 96.0f);
    if (borderWidth < 1.0f)
        borderWidth = 1.0f;

    GLint locBorderWidth = glGetUniformLocation(m_program, "u_borderWidth");
    glUniform1f(locBorderWidth, borderWidth);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glUniform1i(glGetUniformLocation(m_program, "u_texture"), 0);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void PreviewWindowImpl::CleanupSurface() {
    if (m_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_surface != EGL_NO_SURFACE) {
            eglDestroySurface(m_display, m_surface);
            m_surface = EGL_NO_SURFACE;
        }
    }
}

void PreviewWindowImpl::CleanupGL() {
    // Requires context to be current to delete resources
    if (m_display != EGL_NO_DISPLAY && m_context != EGL_NO_CONTEXT) {
        // Use dummy surface to make current and delete GL objects
        if (m_dummySurface != EGL_NO_SURFACE) {
            eglMakeCurrent(m_display, m_dummySurface, m_dummySurface, m_context);
            if (m_program) glDeleteProgram(m_program);
            if (m_vbo) glDeleteBuffers(1, &m_vbo);
        }
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(m_display, m_context);
        m_context = EGL_NO_CONTEXT;
    }
}

