#include "PreviewModule.h"
#include "Logger.h"
#include "SystemInfo.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>

#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

class PreviewWindowImpl : public PreviewWindow {
public:
  PreviewWindowImpl() {}
  ~PreviewWindowImpl() { Cleanup(); }

  SelectionRect Show(std::shared_ptr<CapturedFrame> frame) override {
    LOG("PreviewWindow::Show called.");
    m_frame = frame;
    m_hdrInfo = SystemInfo::GetPrimaryDisplayHdrInfo();
    LOG("HDR Info: SDR White Level=" + std::to_string(m_hdrInfo.sdrWhiteLevel));

    LOG("Creating Win32 window...");
    if (!CreateWin32Window()) {
      LOG("Failed to create Win32 window.");
      return {};
    }
    LOG("Window created.");

    LOG("Initializing EGL...");
    if (!InitEGL()) {
      LOG("Failed to initialize EGL.");
      return {};
    }
    LOG("EGL initialized.");

    LOG("Initializing GL...");
    if (!InitGL()) {
      LOG("Failed to initialize GL.");
      return {};
    }
    LOG("GL initialized.");

    m_running = true;
    m_selectionConfirmed = false;

    // Center the initial selection (optional, or start empty)
    m_selection = {0, 0, 0, 0};

    LOG("Entering message loop...");
    MSG msg;
    while (m_running) {
      while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
          LOG("WM_QUIT received.");
          m_running = false;
          break;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
      }

      Render();
      if (!eglSwapBuffers(m_display, m_surface)) {
        LOG("eglSwapBuffers failed.");
      }

      // Reduce CPU usage if not moving/changing, but for a screenshot tool
      // simple 60fps or on-demand is fine.
      Sleep(16);
    }

    LOG("Exiting message loop. Cleaning up...");
    Cleanup();
    LOG("Cleanup done. Show returning.");
    return m_selectionConfirmed ? m_selection : SelectionRect{0, 0, 0, 0};
  }

private:
  bool CreateWin32Window();
  bool InitEGL();
  bool InitGL();
  void Render();
  void Cleanup();

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                  LPARAM lParam);

  HWND m_hwnd = nullptr;
  EGLDisplay m_display = EGL_NO_DISPLAY;
  EGLConfig m_config = nullptr;
  EGLSurface m_surface = EGL_NO_SURFACE;
  EGLContext m_context = EGL_NO_CONTEXT;

  std::shared_ptr<CapturedFrame> m_frame;
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

  int GetDragMode(int x, int y) {
    if (!m_selection.IsValid())
      return 0;

    int l = m_selection.Left();
    int t = m_selection.Top();
    int r = m_selection.Right();
    int b = m_selection.Bottom();

    const int tolerance = 5;
    const int tol = 3;

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

std::unique_ptr<PreviewWindow> PreviewWindow::Create() {
  return std::make_unique<PreviewWindowImpl>();
}

LRESULT CALLBACK PreviewWindowImpl::WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                            LPARAM lParam) {
  PreviewWindowImpl *self =
      (PreviewWindowImpl *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

  switch (msg) {
  case WM_CREATE: {
    LOG("WM_CREATE received.");
    CREATESTRUCT *cs = (CREATESTRUCT *)lParam;
    SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    return 0;
  }
  case WM_KEYDOWN:
    LOG("WM_KEYDOWN: " + std::to_string(wParam));
    if (wParam == VK_ESCAPE) {
      self->m_selectionConfirmed = false;
      self->m_running = false;
    } else if (wParam == VK_RETURN) {
      self->m_selectionConfirmed = true;
      self->m_running = false;
    }
    return 0;
  case WM_LBUTTONDOWN: {
    LOG("WM_LBUTTONDOWN");
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
    } else {
      // Update cursor feedback
      int mode = self->GetDragMode(x, y);
      LPCWSTR cursor = (LPCWSTR)IDC_CROSS;
      switch (mode) {
      case 1:
      case 3:
        cursor = (LPCWSTR)IDC_SIZENWSE;
        break;
      case 2:
      case 4:
        cursor = (LPCWSTR)IDC_SIZENESW;
        break;
      case 5:
      case 7:
        cursor = (LPCWSTR)IDC_SIZENS;
        break;
      case 6:
      case 8:
        cursor = (LPCWSTR)IDC_SIZEWE;
        break;
      case 9:
        cursor = (LPCWSTR)IDC_SIZEALL;
        break;
      default:
        cursor = (LPCWSTR)IDC_CROSS;
        break;
      }
      SetCursor(LoadCursorW(nullptr, cursor));
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
  case WM_LBUTTONUP: {
    LOG("WM_LBUTTONUP");
    self->m_isDragging = false;
    return 0;
  }
  case WM_RBUTTONUP: {
    LOG("WM_RBUTTONUP");
    self->m_selectionConfirmed = true;
    self->m_running = false;
    return 0;
  }
  case WM_DESTROY:
    LOG("WM_DESTROY received.");
    self->m_running = false;
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool PreviewWindowImpl::CreateWin32Window() {
  WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = L"PrintscrPreview";
  wc.hCursor = LoadCursor(nullptr, IDC_CROSS);
  wc.style = CS_DBLCLKS;

  RegisterClassExW(&wc);

  m_windowWidth = GetSystemMetrics(SM_CXSCREEN);
  m_windowHeight = GetSystemMetrics(SM_CYSCREEN);

  m_hwnd = CreateWindowExW(
      WS_EX_TOPMOST, L"PrintscrPreview", L"Preview", WS_POPUP | WS_VISIBLE, 0,
      0, m_windowWidth, m_windowHeight, nullptr, nullptr, wc.hInstance, this);

  return m_hwnd != nullptr;
}

bool PreviewWindowImpl::InitEGL() {
  m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (m_display == EGL_NO_DISPLAY)
    return false;

  if (!eglInitialize(m_display, nullptr, nullptr))
    return false;

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
  if (!eglChooseConfig(m_display, configAttribs, &m_config, 1, &numConfigs) ||
      numConfigs == 0) {
    // Fallback if float config fails (though it shouldn't for HDR)
    return false;
  }

  EGLint contextAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  m_context =
      eglCreateContext(m_display, m_config, EGL_NO_CONTEXT, contextAttribs);
  if (m_context == EGL_NO_CONTEXT)
    return false;

  EGLint surfaceAttribs[] = {EGL_DIRECT_COMPOSITION_ANGLE, TRUE, EGL_NONE};
  m_surface =
      eglCreateWindowSurface(m_display, m_config, m_hwnd, surfaceAttribs);
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

    if (inside) {
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
      -1.0f, 1.0f, 0.0f, 0.0f, -1.0f, -1.0f, 0.0f, 1.0f,
      1.0f,  1.0f, 1.0f, 0.0f, 1.0f,  -1.0f, 1.0f, 1.0f,
  };
  glGenBuffers(1, &m_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

  // Texture
  glGenTextures(1, &m_texture);
  glBindTexture(GL_TEXTURE_2D, m_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  // Upload FP16 data (RGBA16F)
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, m_frame->metadata.width,
               m_frame->metadata.height, 0, GL_RGBA, GL_HALF_FLOAT,
               m_frame->pixelData.data());

  return true;
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

  glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        (void *)(2 * sizeof(float)));

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_texture);
  glUniform1i(glGetUniformLocation(m_program, "u_texture"), 0);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void PreviewWindowImpl::Cleanup() {
  if (m_program)
    glDeleteProgram(m_program);
  if (m_texture)
    glDeleteTextures(1, &m_texture);
  if (m_vbo)
    glDeleteBuffers(1, &m_vbo);

  if (m_display != EGL_NO_DISPLAY) {
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (m_surface != EGL_NO_SURFACE)
      eglDestroySurface(m_display, m_surface);
    if (m_context != EGL_NO_CONTEXT)
      eglDestroyContext(m_display, m_context);
    eglTerminate(m_display);
  }

  if (m_hwnd)
    DestroyWindow(m_hwnd);

  m_hwnd = nullptr;
  m_display = EGL_NO_DISPLAY;
  m_surface = EGL_NO_SURFACE;
  m_context = EGL_NO_CONTEXT;
}
