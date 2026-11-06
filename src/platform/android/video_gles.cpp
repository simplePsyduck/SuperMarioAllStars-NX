// Video on Android: see video_gles.h.
#include "platform/android/video_gles.h"
#include <SDL.h>
#include <GLES3/gl3.h>
#ifdef __SWITCH__
#include "platform/switch/glyphs_nx.h"
#include "platform/switch/nx_sys.h"  // (not libnx itself: only nx_sys.cpp includes <switch.h>)
#else
#include <android/log.h>
#include <jni.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#ifdef __SWITCH__
void logf(const char* fmt, ...);  // (the app's log file, smas_port.log: see app/app.cpp)
#endif

namespace {

#ifdef __SWITCH__
// (stderr goes nowhere on a Switch: the shader and GL messages go to smas_port.log)
void logError(const char* msg) { logf("GL error: %s", msg); }
void logInfo(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  logf("%s", buf);
}
// Reports (and clears) what the driver has flagged since the last look; the log keeps the first few only.
void checkGl(const char* where) {
  static int budget = 40;
  for (int i = 0; i < 8; i++) {
    GLenum e = glGetError();
    if (e == GL_NO_ERROR) break;
    if (budget > 0) {
      budget--;
      logInfo("glGetError 0x%04x after %s", (unsigned)e, where);
    }
  }
}
#else
void logError(const char* msg) { __android_log_print(ANDROID_LOG_ERROR, "SmasPort", "%s", msg); }
#endif

GLuint compile(GLenum type, const std::string& src) {
  GLuint s = glCreateShader(type);
  const char* p = src.c_str();
  glShaderSource(s, 1, &p, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char buf[2048];
    glGetShaderInfoLog(s, sizeof(buf), nullptr, buf);
    logError(buf);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

GLuint linkProgram(const std::string& vs, const std::string& fs) {
  GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) return 0;
  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok || !glIsProgram(p)) {
    char buf[2048];
    glGetProgramInfoLog(p, sizeof(buf), nullptr, buf);
    logError(buf);
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

// ---- the game picture: app/video.cpp's shader in GLSL ------------------------
const char* kGameVS = R"(#version 300 es
out vec2 v_uv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  gl_Position = vec4(p * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
  v_uv = p;
}
)";

const char* kGameFS = R"(#version 300 es
precision highp float;
precision highp int;
uniform sampler2D u_texP;   // the picture, point sampled
uniform sampler2D u_texL;   // ... and linear
uniform vec4 u_texSize;     // w, h, 1/w, 1/h
uniform vec4 u_outSize;     // viewport w, h, 1/w, 1/h
uniform vec4 u_f0;          // filter, scanlines, curve, pass
uniform vec4 u_f1;          // brightness, contrast, saturation, gamma
uniform vec4 u_f2;          // profile, colorblind, src w, src h
uniform vec4 u_f3;          // fade, pillarbox, temperature, 0
in vec2 v_uv;
out vec4 o_col;

vec2 clampUv(vec2 uv) { return clamp(uv, 0.5 * u_texSize.zw, vec2(1.0) - 0.5 * u_texSize.zw); }
vec3 texNearest(vec2 uv) { return texture(u_texP, clampUv(uv)).bgr; }
vec3 texLinear(vec2 uv) { return texture(u_texL, clampUv(uv)).bgr; }

vec3 texSharp(vec2 uv) {
  vec2 texel = uv * u_texSize.xy;
  vec2 scale = max(floor(u_outSize.xy / u_f2.zw), vec2(1.0));
  vec2 tf = floor(texel), s = fract(texel);
  vec2 region = 0.5 - 0.5 / scale;
  vec2 cd = s - 0.5;
  vec2 f = (cd - clamp(cd, -region, region)) * scale + 0.5;
  return texLinear((tf + f) * u_texSize.zw);
}

vec3 texRow(float x, float row) {
  vec2 scale = max(floor(u_outSize.xy / u_f2.zw), vec2(1.0));
  float tf = floor(x), s = fract(x);
  float region = 0.5 - 0.5 / max(scale.x, 1.0);
  float cd = s - 0.5;
  float f = (cd - clamp(cd, -region, region)) * max(scale.x * 0.6, 1.0) + 0.5;
  return texLinear(vec2((tf + f) * u_texSize.z, (row + 0.5) * u_texSize.w));
}

#ifndef SMAS_LITE
vec3 crt(vec2 uv, vec2 pos, bool aperture) {
  vec2 texel = uv * u_texSize.xy;
  float y = texel.y - 0.5, yf = floor(y), t = y - yf;
  vec3 c0 = texRow(texel.x, yf), c1 = texRow(texel.x, yf + 1.0);
  c0 = c0 * c0;
  c1 = c1 * c1;
  float strength = u_f0.y;
  vec3 w0 = exp(-t * t * mix(3.0, 12.0, strength) / (0.6 + 0.8 * sqrt(c0)));
  vec3 w1 = exp(-(1.0 - t) * (1.0 - t) * mix(3.0, 12.0, strength) / (0.6 + 0.8 * sqrt(c1)));
  vec3 col = c0 * w0 + c1 * w1;
  col *= mix(1.0, 1.55, strength);
  if (aperture) {
    int m = int(mod(pos.x, 3.0));
    vec3 mask = m == 0 ? vec3(1.0, 0.7, 0.7) : (m == 1 ? vec3(0.7, 1.0, 0.7) : vec3(0.7, 0.7, 1.0));
    col *= mix(vec3(1.0), mask * 1.25, strength);
  }
  return sqrt(max(col, 0.0));
}

vec3 lcd(vec2 uv) {
  vec2 texel = uv * u_texSize.xy;
  vec2 f = fract(texel);
  vec3 c = texNearest(uv);
  vec2 scale = u_outSize.xy / u_f2.zw;
  vec2 edge = smoothstep(vec2(0.0), 1.5 / scale, f) * smoothstep(vec2(0.0), 1.5 / scale, 1.0 - f);
  float g = mix(1.0, edge.x * edge.y, 0.55 * u_f0.y + 0.2);
  return c * g;
}
#endif

vec3 applyProfile(vec3 c) {
  int prof = int(u_f2.x);
  float l = dot(c, vec3(0.299, 0.587, 0.114));
  if (prof == 1) {
    c = mix(vec3(l), c, 1.3);
    c = (c - 0.5) * 1.08 + 0.5;
  } else if (prof == 2) {
    c = pow(max(c, 0.0), vec3(1.15));
    c = c * mat3(1.02, 0.02, -0.04, 0.0, 1.0, 0.0, -0.02, 0.03, 0.95);
  } else if (prof == 3) {
    c = mix(vec3(l), c, 0.72);
    c = (c - 0.5) * 0.9 + 0.52;
  } else if (prof == 4) {
    c = vec3(l);
  } else if (prof == 5) {
    c = vec3(dot(c, vec3(0.393, 0.769, 0.189)), dot(c, vec3(0.349, 0.686, 0.168)), dot(c, vec3(0.272, 0.534, 0.131)));
  } else if (prof == 6) {
    float q = floor(clamp(l, 0.0, 1.0) * 3.999);
    c = q < 1.0 ? vec3(0.06, 0.22, 0.06) : q < 2.0 ? vec3(0.19, 0.38, 0.19) : q < 3.0 ? vec3(0.55, 0.67, 0.06)
                                                                                : vec3(0.61, 0.74, 0.06);
  } else if (prof == 7) {
    c = vec3(pow(clamp(l, 0.0, 1.0), 0.8), 0.0, 0.0);
  } else if (prof == 8) {
    c = c * vec3(1.0, 0.86, 0.62);
  } else if (prof == 9) {
    c = pow(max(c, 0.0), vec3(1.18)) * 1.04;
    c = mix(vec3(l * 1.02), c, 0.92);
  }
  return c;
}

#ifndef SMAS_LITE
vec3 daltonize(vec3 c, int mode) {
  mat3 rgb2lms = mat3(17.8824, 43.5161, 4.11935, 3.45565, 27.1554, 3.86714, 0.0299566, 0.184309, 1.46709);
  mat3 lms2rgb = mat3(0.0809444479, -0.130504409, 0.116721066, -0.0102485335, 0.0540193266, -0.113614708,
                      -0.000365296938, -0.00412161469, 0.693511405);
  vec3 lms = c * rgb2lms;
  vec3 s;
  if (mode == 1) s = vec3(2.02344 * lms.y - 2.52581 * lms.z, lms.y, lms.z);
  else if (mode == 2) s = vec3(lms.x, 0.494207 * lms.x + 1.24827 * lms.z, lms.z);
  else s = vec3(lms.x, lms.y, -0.395913 * lms.x + 0.801109 * lms.y);
  vec3 sim = s * lms2rgb;
  vec3 err = c - sim;
  vec3 corr = vec3(0.0, err.r * 0.7 + err.g, err.r * 0.7 + err.b);
  return clamp(c + corr, 0.0, 1.0);
}
#endif

vec3 applyColor(vec3 c) {
  c = pow(max(c, 0.0), vec3(1.0 / u_f1.w));
  c = (c - 0.5) * u_f1.y + 0.5;
  c *= u_f1.x;
  float l = dot(c, vec3(0.299, 0.587, 0.114));
  c = mix(vec3(l), c, u_f1.z);
  c = applyProfile(clamp(c, 0.0, 1.0));
  float tmp = u_f3.z;
  if (tmp > 0.0) c *= mix(vec3(1.0), vec3(1.0, 0.9, 0.74), tmp);
  else if (tmp < 0.0) c *= mix(vec3(1.0), vec3(0.82, 0.92, 1.0), -tmp);
#ifndef SMAS_LITE
  int cb = int(u_f2.y);
  if (cb > 0) c = daltonize(c, cb);
#endif
  return clamp(c, 0.0, 1.0);
}

void main() {
  vec2 uv = v_uv;
#ifdef SMAS_LITE
  if (int(u_f0.w) == 1) {
    o_col = vec4(0.0, 0.0, 0.0, 1.0);
    return;
  }
#else
  if (int(u_f0.w) == 1) {
    // a blurred, darkened backdrop for the sides
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int y = -3; y <= 3; y++)
      for (int x = -3; x <= 3; x++) {
        float w = exp(-float(x * x + y * y) / 8.0);
        acc += texLinear(uv + vec2(float(x) * 6.0, float(y) * 4.0) * u_texSize.zw) * w;
        wsum += w;
      }
    vec3 c = applyColor(acc / wsum) * 0.32;
    float vign = 1.0 - 0.5 * pow(abs(uv.x * 2.0 - 1.0), 3.0);
    o_col = vec4(c * vign * u_f3.x, 1.0);
    return;
  }
#endif
  float curve = u_f0.z;
  if (curve > 0.0) {
    vec2 cc = uv * 2.0 - 1.0;
    cc *= 1.0 + (cc.yx * cc.yx) * curve * 0.12;
    uv = cc * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
      o_col = vec4(0.0, 0.0, 0.0, 1.0);
      return;
    }
  }
  int filt = int(u_f0.x);
  vec3 c;
#ifdef SMAS_LITE
  if (filt == 0) c = texNearest(uv);
  else if (filt == 2) c = texLinear(uv);
  else c = texSharp(uv);
#else
  if (filt == 0) c = texNearest(uv);
  else if (filt == 1) c = texSharp(uv);
  else if (filt == 2) c = texLinear(uv);
  else if (filt == 3) c = crt(uv, gl_FragCoord.xy, false);
  else if (filt == 4) c = crt(uv, gl_FragCoord.xy, true);
  else c = lcd(uv);
#endif
  c = applyColor(c);
  if (curve > 0.0) {
    vec2 cc = uv * 2.0 - 1.0;
    c *= clamp((1.0 - cc.x * cc.x) * (1.0 - cc.y * cc.y) * 12.0, 0.0, 1.0);
  }
  o_col = vec4(c * u_f3.x, 1.0);
}
)";

// ---- the UI: batched quads ---------------------------------------------------
const char* kOverlayVS = R"(#version 300 es
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
layout(location = 3) in vec4 a_prm;
layout(location = 4) in float a_mode;
uniform vec2 u_screen;   // 2/W, 2/H
out vec2 v_uv;
out vec4 v_col;
flat out vec4 v_prm;
flat out float v_mode;
void main() {
  gl_Position = vec4(a_pos.x * u_screen.x - 1.0, 1.0 - a_pos.y * u_screen.y, 0.0, 1.0);
  v_uv = a_uv;
  v_col = a_col;
  v_prm = a_prm;
  v_mode = a_mode;
}
)";

const char* kOverlayFS = R"(#version 300 es
precision highp float;
uniform sampler2D u_atlas;   // glyph coverage (unit 0)
uniform sampler2D u_image;   // a picture (unit 1)
in vec2 v_uv;
in vec4 v_col;
flat in vec4 v_prm;
flat in float v_mode;
out vec4 o_col;
void main() {
  float a = 1.0;
  if (v_mode > 3.5) {          // a picture (0x00RRGGBB, uploaded as RGBA: B G R in r g b; 5: its alpha too)
    vec4 t = texture(u_image, v_uv);
    o_col = vec4(t.bgr, v_mode > 4.5 ? t.a * v_col.a : v_col.a);
    return;
  } else if (v_mode > 1.5) {   // rounded rect SDF; uv = offset from the centre, prm = hw, hh, r, stroke
    vec2 q = abs(v_uv) - v_prm.xy + v_prm.z;
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - v_prm.z;
    a = clamp(0.5 - d, 0.0, 1.0);
    if (v_prm.w > 0.0) a *= clamp(d + v_prm.w + 0.5, 0.0, 1.0);
  } else if (v_mode > 0.5) {   // glyph coverage
    a = texture(u_atlas, v_uv).r;
  }
  o_col = vec4(v_col.rgb, v_col.a * a);
}
)";

struct OVert {
  float x, y, u, v;
  uint32_t col;
  float p[4];
  float mode;
};
static_assert(sizeof(OVert) == 40, "vertex layout");
const int kMaxQuads = 16384;  // per draw call (16-bit indices)

uint32_t rgba(ColorF c) {
  auto b = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
  return b(c.r) | b(c.g) << 8 | b(c.b) << 16 | b(c.a) << 24;
}

// The text's code points: ASCII, Latin-1 and the symbols the menus use.
const uint32_t kExtraCps[] = {0x2190, 0x2191, 0x2192, 0x2193, 0x25B6, 0x25C0, 0x2713, 0x2022, 0x266A, 0x2026,
                              0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2605, 0x24B6, 0x24B7, 0x2122,
                              0x00D7, 0x2715};
// Font sizes glyphs are made at (others drawn scaled from the nearest).
const float kSizes[] = {9, 11, 13, 15, 17, 19, 22, 25, 28, 32, 36, 41, 47, 54, 62, 71, 82, 94, 108, 124, 142};

}  // namespace

// ---------------------------------------------------------------------------
class GlesCanvas : public Canvas {
public:
  explicit GlesCanvas(unsigned epoch) : epoch_(epoch) {}
  ~GlesCanvas() override { release(); }
  bool create();
  void release();
  void begin(int w, int h);
  void end() { flush(); }

  unsigned epoch() const override { return epoch_; }
  void fillRect(RectF r, ColorF c) override {
    uint32_t k = rgba(c);
    quad(r.left, r.top, r.right, r.bottom, k, k, 0, 0, 0, 0, nullptr, 0);
  }
  void strokeRect(RectF r, float width, ColorF c) override { roundRect(grow(r, width * 0.5f), 0, width, c); }
  void fillRound(RectF r, float radius, ColorF c) override { roundRect(r, radius, 0, c); }
  void strokeRound(RectF r, float radius, float width, ColorF c) override {
    roundRect(grow(r, width * 0.5f), radius + width * 0.5f, width, c);
  }
  void strokeEllipse(float cx, float cy, float rx, float ry, float width, ColorF c) override {
    strokeRound(rectF(cx - rx, cy - ry, cx + rx, cy + ry), std::min(rx, ry), width, c);
  }
  void fillGradient(RectF r, ColorF top, ColorF bottom) override {
    quad(r.left, r.top, r.right, r.bottom, rgba(top), rgba(bottom), 0, 0, 0, 0, nullptr, 0);
  }
  void fillPolygon(const PointF* pts, int n, ColorF c) override {
    if (n < 3) return;
    float mx = 0, my = 0;
    for (int i = 0; i < n; i++) mx += pts[i].x, my += pts[i].y;
    mx /= n;
    my /= n;
    uint32_t k = rgba(c);
    for (int i = 0; i < n; i++) {
      const PointF &a = pts[i], &b = pts[(i + 1) % n];
      tri(mx, my, a.x, a.y, b.x, b.y, k);
    }
  }
  void strokePolygon(const PointF* pts, int n, float width, ColorF c) override {
    uint32_t k = rgba(c);
    for (int i = 0; i < n; i++) line(pts[i], pts[(i + 1) % n], width, k);
  }
  int makeShape(const std::vector<PointF>& pts, const std::vector<int>& counts) override;
  void fillShape(int shape, ColorF c, float scale, float tx, float ty) override;
  void freeShape(int shape) override {
    if (shape >= 1 && shape <= (int)shapes_.size()) shapes_[shape - 1].clear();
  }
  void text(const std::wstring& s, RectF r, float size, int weight, ColorF c, TextAlign align,
            ParaAlign para) override;
  float measure(const std::wstring& s, float size, int weight) override;
  void titleText(const std::wstring& s, RectF r, float size, ColorF c, TextAlign align, ParaAlign para) override {
    drawText(s, r, size, kWeightNormal, true, c, align, para);
  }
  float measureTitle(const std::wstring& s, float size) override { return measureText(s, size, kWeightNormal, true); }
  void pushClip(RectF r) override;
  void popClip() override;
  int makeImage(int w, int h, bool alpha) override;
  void updateImage(int image, const uint32_t* px, int stridePixels) override;
  void drawImage(int image, RectF dst, float opacity, bool smooth) override;
  void freeImage(int image) override;

private:
  unsigned epoch_;
  int w_ = 0, h_ = 0;
  GLuint prog_ = 0, vao_ = 0, vb_ = 0, ib_ = 0;
  GLint locScreen_ = -1, locAtlas_ = -1, locImage_ = -1;
  std::vector<OVert> verts_;
  GLuint boundAtlas_ = 0;
  std::vector<RectF> clips_;
  std::vector<std::vector<float>> shapes_;  // triangles: x, y per corner
  struct Image {
    GLuint tex = 0;
    int w = 0, h = 0;
    bool alpha = false;
  };
  std::vector<Image> images_;
  struct Glyph {
    float adv;
    int ox, oy, w, h;
    float u0, v0, u1, v1;
  };
  struct Font {
    GLuint tex = 0;
    int ascent = 0, lineH = 0;
    std::map<uint32_t, Glyph> glyphs;
    bool ok = false, tried = false;
  };
  std::map<int, Font> fonts_;  // by size index * 4 + title * 2 + bold

  static RectF grow(RectF r, float d) { return rectF(r.left - d, r.top - d, r.right + d, r.bottom + d); }
  void flush();
  void useAtlas(GLuint tex) {
    if (tex != boundAtlas_) {
      flush();
      boundAtlas_ = tex;
    }
  }
  void push4(const OVert v[4]);
  void quad(float x0, float y0, float x1, float y1, uint32_t ctop, uint32_t cbot, float u0, float v0, float u1,
            float v1, const float* prm, float mode);
  void tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c);
  void line(PointF a, PointF b, float width, uint32_t c);
  void roundRect(RectF r, float radius, float stroke, ColorF c);
  Font& font(float size, int weight, bool title, float* scale);
  bool buildFont(Font& f, float px, bool bold, bool title);
  void drawText(const std::wstring& s, RectF r, float size, int weight, bool title, ColorF c, TextAlign align,
                ParaAlign para);
  float measureText(const std::wstring& s, float size, int weight, bool title);
  void applyScissor();
};

bool GlesCanvas::create() {
  prog_ = linkProgram(kOverlayVS, kOverlayFS);
  if (!prog_) return false;
  locScreen_ = glGetUniformLocation(prog_, "u_screen");
  locAtlas_ = glGetUniformLocation(prog_, "u_atlas");
  locImage_ = glGetUniformLocation(prog_, "u_image");
  glGenVertexArrays(1, &vao_);
  glBindVertexArray(vao_);
  glGenBuffers(1, &vb_);
  glBindBuffer(GL_ARRAY_BUFFER, vb_);
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)kMaxQuads * 4 * sizeof(OVert), nullptr, GL_STREAM_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(OVert), (void*)offsetof(OVert, x));
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(OVert), (void*)offsetof(OVert, u));
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(OVert), (void*)offsetof(OVert, col));
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(OVert), (void*)offsetof(OVert, p));
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(OVert), (void*)offsetof(OVert, mode));
  std::vector<uint16_t> idx(kMaxQuads * 6);
  for (int q = 0; q < kMaxQuads; q++) {
    uint16_t b = (uint16_t)(q * 4);
    uint16_t* o = &idx[q * 6];
    o[0] = b;
    o[1] = (uint16_t)(b + 1);
    o[2] = (uint16_t)(b + 2);
    o[3] = (uint16_t)(b + 2);
    o[4] = (uint16_t)(b + 1);
    o[5] = (uint16_t)(b + 3);
  }
  glGenBuffers(1, &ib_);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib_);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(idx.size() * 2), idx.data(), GL_STATIC_DRAW);
  glBindVertexArray(0);
  return true;
}

void GlesCanvas::release() {
  for (auto& kv : fonts_)
    if (kv.second.tex) glDeleteTextures(1, &kv.second.tex);
  fonts_.clear();
  for (Image& i : images_)
    if (i.tex) glDeleteTextures(1, &i.tex);
  images_.clear();
  if (prog_) glDeleteProgram(prog_);
  if (vb_) glDeleteBuffers(1, &vb_);
  if (ib_) glDeleteBuffers(1, &ib_);
  if (vao_) glDeleteVertexArrays(1, &vao_);
  prog_ = vb_ = ib_ = vao_ = 0;
}

void GlesCanvas::begin(int w, int h) {
  w_ = w;
  h_ = h;
  verts_.clear();
  clips_.clear();
  boundAtlas_ = 0;
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void GlesCanvas::flush() {
  if (verts_.empty() || !prog_) {
    verts_.clear();
    return;
  }
  glUseProgram(prog_);
  glUniform2f(locScreen_, 2.0f / (float)w_, 2.0f / (float)h_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, boundAtlas_);
  glUniform1i(locAtlas_, 0);
  glUniform1i(locImage_, 1);
  glBindVertexArray(vao_);
  glBindBuffer(GL_ARRAY_BUFFER, vb_);
#ifdef __SWITCH__
  // (a new buffer of just the size used: the driver is not asked for 2.6 MB at every flush)
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts_.size() * sizeof(OVert)), verts_.data(), GL_STREAM_DRAW);
#else
  glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)kMaxQuads * 4 * sizeof(OVert), nullptr, GL_STREAM_DRAW);  // orphan
  glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(verts_.size() * sizeof(OVert)), verts_.data());
#endif
  glDrawElements(GL_TRIANGLES, (GLsizei)(verts_.size() / 4 * 6), GL_UNSIGNED_SHORT, nullptr);
  glBindVertexArray(0);
  verts_.clear();
}

void GlesCanvas::push4(const OVert v[4]) {
  if (verts_.size() >= (size_t)kMaxQuads * 4) flush();
  verts_.insert(verts_.end(), v, v + 4);
}

void GlesCanvas::quad(float x0, float y0, float x1, float y1, uint32_t ctop, uint32_t cbot, float u0, float v0,
                      float u1, float v1, const float* prm, float mode) {
  if (x1 <= x0 || y1 <= y0) return;
  float p[4] = {0, 0, 0, 0};
  if (prm) std::memcpy(p, prm, sizeof(p));
  OVert v[4] = {
      {x0, y0, u0, v0, ctop, {p[0], p[1], p[2], p[3]}, mode},
      {x1, y0, u1, v0, ctop, {p[0], p[1], p[2], p[3]}, mode},
      {x0, y1, u0, v1, cbot, {p[0], p[1], p[2], p[3]}, mode},
      {x1, y1, u1, v1, cbot, {p[0], p[1], p[2], p[3]}, mode},
  };
  push4(v);
}

// (a triangle as a quad whose last corner repeats the third: its second triangle is empty)
void GlesCanvas::tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c) {
  OVert v[4] = {
      {x0, y0, 0, 0, c, {0, 0, 0, 0}, 0},
      {x1, y1, 0, 0, c, {0, 0, 0, 0}, 0},
      {x2, y2, 0, 0, c, {0, 0, 0, 0}, 0},
      {x2, y2, 0, 0, c, {0, 0, 0, 0}, 0},
  };
  push4(v);
}

void GlesCanvas::line(PointF a, PointF b, float width, uint32_t c) {
  float dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
  if (len <= 0) return;
  float nx = -dy / len * width * 0.5f, ny = dx / len * width * 0.5f;
  OVert v[4] = {
      {a.x + nx, a.y + ny, 0, 0, c, {0, 0, 0, 0}, 0},
      {b.x + nx, b.y + ny, 0, 0, c, {0, 0, 0, 0}, 0},
      {a.x - nx, a.y - ny, 0, 0, c, {0, 0, 0, 0}, 0},
      {b.x - nx, b.y - ny, 0, 0, c, {0, 0, 0, 0}, 0},
  };
  push4(v);
}

void GlesCanvas::roundRect(RectF r, float radius, float stroke, ColorF c) {
  float w = r.right - r.left, h = r.bottom - r.top;
  if (w <= 0 || h <= 0) return;
  float hw = w * 0.5f, hh = h * 0.5f;
  float prm[4] = {hw, hh, std::max(0.0f, std::min(radius, std::min(hw, hh))), stroke};
  float cx = r.left + hw, cy = r.top + hh;
  float x0 = r.left - 1, y0 = r.top - 1, x1 = r.right + 1, y1 = r.bottom + 1;
  uint32_t k = rgba(c);
  quad(x0, y0, x1, y1, k, k, x0 - cx, y0 - cy, x1 - cx, y1 - cy, prm, 2.0f);
}

int GlesCanvas::makeShape(const std::vector<PointF>& pts, const std::vector<int>& counts) {
  std::vector<float> t;
  size_t at = 0;
  for (int n : counts) {
    if (n >= 3) {
      float mx = 0, my = 0;
      for (int i = 0; i < n; i++) mx += pts[at + i].x, my += pts[at + i].y;
      mx /= n;
      my /= n;
      for (int i = 0; i < n; i++) {
        const PointF &a = pts[at + i], &b = pts[at + (i + 1) % n];
        t.insert(t.end(), {mx, my, a.x, a.y, b.x, b.y});
      }
    }
    at += n;
  }
  for (size_t i = 0; i < shapes_.size(); i++)
    if (shapes_[i].empty()) {
      shapes_[i] = std::move(t);
      return (int)i + 1;
    }
  shapes_.push_back(std::move(t));
  return (int)shapes_.size();
}

void GlesCanvas::fillShape(int shape, ColorF c, float scale, float tx, float ty) {
  if (shape < 1 || shape > (int)shapes_.size()) return;
  const std::vector<float>& t = shapes_[shape - 1];
  uint32_t k = rgba(c);
  for (size_t i = 0; i + 6 <= t.size(); i += 6)
    tri(t[i] * scale + tx, t[i + 1] * scale + ty, t[i + 2] * scale + tx, t[i + 3] * scale + ty,
        t[i + 4] * scale + tx, t[i + 5] * scale + ty, k);
}

// ---- clipping ----------------------------------------------------------------
void GlesCanvas::applyScissor() {
  if (clips_.empty()) {
    glDisable(GL_SCISSOR_TEST);
    return;
  }
  RectF r = clips_.back();
  int x0 = (int)std::floor(std::max(0.0f, r.left)), y0 = (int)std::floor(std::max(0.0f, r.top));
  int x1 = (int)std::ceil(std::min((float)w_, r.right)), y1 = (int)std::ceil(std::min((float)h_, r.bottom));
  glEnable(GL_SCISSOR_TEST);
  glScissor(x0, h_ - std::max(y0, y1), std::max(0, x1 - x0), std::max(0, y1 - y0));
}

void GlesCanvas::pushClip(RectF r) {
  flush();
  if (!clips_.empty()) {
    const RectF& p = clips_.back();
    r = rectF(std::max(r.left, p.left), std::max(r.top, p.top), std::min(r.right, p.right),
              std::min(r.bottom, p.bottom));
  }
  clips_.push_back(r);
  applyScissor();
}

void GlesCanvas::popClip() {
  flush();
  if (!clips_.empty()) clips_.pop_back();
  applyScissor();
}

// ---- pictures ----------------------------------------------------------------
int GlesCanvas::makeImage(int w, int h, bool alpha) {
  Image im;
  im.w = w;
  im.h = h;
  im.alpha = alpha;
  glGenTextures(1, &im.tex);
  glBindTexture(GL_TEXTURE_2D, im.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  for (size_t i = 0; i < images_.size(); i++)
    if (!images_[i].tex) {
      images_[i] = im;
      return (int)i + 1;
    }
  images_.push_back(im);
  return (int)images_.size();
}

void GlesCanvas::updateImage(int image, const uint32_t* px, int stridePixels) {
  if (image < 1 || image > (int)images_.size() || !images_[image - 1].tex) return;
  const Image& im = images_[image - 1];
  glBindTexture(GL_TEXTURE_2D, im.tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, stridePixels);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, im.w, im.h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void GlesCanvas::drawImage(int image, RectF dst, float opacity, bool smooth) {
  if (image < 1 || image > (int)images_.size() || !images_[image - 1].tex) return;
  flush();
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, images_[image - 1].tex);
  GLint f = smooth ? GL_LINEAR : GL_NEAREST;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
  glActiveTexture(GL_TEXTURE0);
  uint32_t k = rgba(colorF(1, 1, 1, opacity));
  quad(dst.left, dst.top, dst.right, dst.bottom, k, k, 0, 0, 1, 1, nullptr, images_[image - 1].alpha ? 5.0f : 4.0f);
  flush();
}

void GlesCanvas::freeImage(int image) {
  if (image < 1 || image > (int)images_.size() || !images_[image - 1].tex) return;
  glDeleteTextures(1, &images_[image - 1].tex);
  images_[image - 1] = Image();
}

// ---- text ----------------------------------------------------------------------
// Glyphs from SmasActivity.renderGlyphs (Android's own text engine; on the Switch
// nxRenderGlyphs, FreeType with the system's font): per code
// point the advance x 64, the bitmap's left and top (from the pen and the
// baseline), its size; then the 8-bit coverage bitmaps one after another.
bool GlesCanvas::buildFont(Font& f, float px, bool bold, bool title) {
  std::vector<uint32_t> cps;
  for (uint32_t c = 0x20; c < 0x7F; c++) cps.push_back(c);
  for (uint32_t c = 0xA1; c < 0x100; c++)
    if (c != 0xAD) cps.push_back(c);
  for (uint32_t c : kExtraCps) cps.push_back(c);
#ifdef __SWITCH__
  std::vector<int> meta;
  std::vector<uint8_t> bits;
  if (!nxRenderGlyphs(cps.data(), cps.size(), px, bold, title, &meta, &bits)) return false;
#else
  JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
  jobject act = (jobject)SDL_AndroidGetActivity();
  if (!env || !act) return false;
  jclass cls = env->GetObjectClass(act);
  jmethodID m = env->GetStaticMethodID(cls, "renderGlyphs", "([IFZZ[I)[B");
  std::vector<int> meta;
  std::vector<uint8_t> bits;
  if (m) {
    jintArray jcps = env->NewIntArray((jsize)cps.size());
    std::vector<jint> tmp(cps.begin(), cps.end());
    env->SetIntArrayRegion(jcps, 0, (jsize)tmp.size(), tmp.data());
    jintArray jmeta = env->NewIntArray((jsize)(2 + cps.size() * 5));
    jbyteArray jbits =
        (jbyteArray)env->CallStaticObjectMethod(cls, m, jcps, (jfloat)px, (jboolean)bold, (jboolean)title, jmeta);
    if (!env->ExceptionCheck() && jbits) {
      meta.resize(2 + cps.size() * 5);
      env->GetIntArrayRegion(jmeta, 0, (jsize)meta.size(), (jint*)meta.data());
      bits.resize((size_t)env->GetArrayLength(jbits));
      env->GetByteArrayRegion(jbits, 0, (jsize)bits.size(), (jbyte*)bits.data());
    }
    env->ExceptionClear();
    if (jbits) env->DeleteLocalRef(jbits);
    env->DeleteLocalRef(jmeta);
    env->DeleteLocalRef(jcps);
  }
  env->ExceptionClear();
  env->DeleteLocalRef(cls);
  env->DeleteLocalRef(act);
#endif
  if (meta.empty()) return false;
  f.ascent = meta[0];
  f.lineH = meta[1];
  // shelf-pack the bitmaps (each has its own margin) into one R8 texture
  struct B {
    uint32_t cp;
    Glyph g;
    size_t off;
  };
  std::vector<B> list;
  size_t off = 0;
  for (size_t i = 0; i < cps.size(); i++) {
    const int* k = &meta[2 + i * 5];
    if (k[0] < 0) continue;
    Glyph g = {k[0] / 64.0f, k[1], k[2], k[3], k[4], 0, 0, 0, 0};
    if (g.w > 0 && off + (size_t)g.w * g.h <= bits.size()) {
      list.push_back({cps[i], g, off});
      off += (size_t)g.w * g.h;
    } else {
      g.w = g.h = 0;
      list.push_back({cps[i], g, 0});
    }
  }
  int aw = 256;
  long long area = 0;
  for (auto& b : list) area += (long long)(b.g.w + 1) * (b.g.h + 1);
  while ((long long)aw * aw < area * 3 / 2 && aw < 4096) aw *= 2;
  int x = 0, y = 0, row = 0;
  std::vector<std::pair<int, int>> at(list.size());
  for (size_t i = 0; i < list.size(); i++) {
    const Glyph& g = list[i].g;
    if (x + g.w + 1 > aw) {
      x = 0;
      y += row;
      row = 0;
    }
    at[i] = {x, y};
    x += g.w + 1;
    row = std::max(row, g.h + 1);
  }
  int ah = std::max(4, (y + row + 3) & ~3);
  std::vector<uint8_t> atlas((size_t)aw * ah, 0);
  for (size_t i = 0; i < list.size(); i++) {
    Glyph g = list[i].g;
    if (g.w > 0 && at[i].second + g.h <= ah) {
      for (int r = 0; r < g.h; r++)
        std::memcpy(&atlas[(size_t)(at[i].second + r) * aw + at[i].first], &bits[list[i].off + (size_t)r * g.w],
                    (size_t)g.w);
      g.u0 = (float)at[i].first / aw;
      g.v0 = (float)at[i].second / ah;
      g.u1 = (float)(at[i].first + g.w) / aw;
      g.v1 = (float)(at[i].second + g.h) / ah;
    }
    f.glyphs[list[i].cp] = g;
  }
  glGenTextures(1, &f.tex);
  glBindTexture(GL_TEXTURE_2D, f.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, aw, ah, 0, GL_RED, GL_UNSIGNED_BYTE, atlas.data());
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  return true;
}

GlesCanvas::Font& GlesCanvas::font(float size, int weight, bool title, float* scale) {
  const int n = (int)(sizeof(kSizes) / sizeof(kSizes[0]));
  int i = 0;
  while (i < n - 1 && kSizes[i] < size * 0.96f) i++;
  *scale = size / kSizes[i];
  const bool bold = weight >= kWeightSemiBold;
  Font& f = fonts_[i * 4 + (title ? 2 : 0) + (bold ? 1 : 0)];
  if (!f.tried) {
    f.tried = true;
    f.ok = buildFont(f, kSizes[i], bold, title);
  }
  return f;
}

float GlesCanvas::measure(const std::wstring& s, float size, int weight) { return measureText(s, size, weight, false); }

float GlesCanvas::measureText(const std::wstring& s, float size, int weight, bool title) {
  if (s.empty() || size <= 0) return 0;
  float scale;
  Font& f = font(size, weight, title, &scale);
  if (!f.ok) return 0;
  float w = 0;
  for (wchar_t wc : s) {
    auto it = f.glyphs.find((uint32_t)wc);
    if (it == f.glyphs.end()) it = f.glyphs.find('?');
    if (it != f.glyphs.end()) w += it->second.adv;
  }
  return w * scale;
}

void GlesCanvas::text(const std::wstring& s, RectF r, float size, int weight, ColorF c, TextAlign align,
                      ParaAlign para) {
  drawText(s, r, size, weight, false, c, align, para);
}

void GlesCanvas::drawText(const std::wstring& s, RectF r, float size, int weight, bool title, ColorF c,
                          TextAlign align, ParaAlign para) {
  if (s.empty() || size <= 0 || r.right <= r.left || r.bottom <= r.top) return;
  float scale;
  Font& f = font(size, weight, title, &scale);
  if (!f.ok) return;
  useAtlas(f.tex);
  const float lw = measureText(s, size, weight, title), lh = f.lineH * scale;
  float pen = align == kAlignCenter ? (r.left + r.right - lw) * 0.5f : align == kAlignTrailing ? r.right - lw : r.left;
  float top = para == kParaCenter ? (r.top + r.bottom - lh) * 0.5f : para == kParaFar ? r.bottom - lh : r.top;
  const float base = std::floor(top + f.ascent * scale + 0.5f);
  const uint32_t k = rgba(c);
  for (wchar_t wc : s) {
    auto it = f.glyphs.find((uint32_t)wc);
    if (it == f.glyphs.end()) it = f.glyphs.find('?');
    if (it == f.glyphs.end()) continue;
    const Glyph& g = it->second;
    if (g.w > 0) {
      float x0 = pen + g.ox * scale, y0 = base + g.oy * scale, x1 = x0 + g.w * scale, y1 = y0 + g.h * scale;
      float u0 = g.u0, v0 = g.v0, u1 = g.u1, v1 = g.v1;
      // (clipped to the rectangle, as DirectWrite's text is)
      if (x0 < r.left) u0 += (u1 - u0) * (r.left - x0) / (x1 - x0), x0 = r.left;
      if (x1 > r.right) u1 -= (u1 - u0) * (x1 - r.right) / (x1 - x0), x1 = r.right;
      if (y0 < r.top) v0 += (v1 - v0) * (r.top - y0) / (y1 - y0), y0 = r.top;
      if (y1 > r.bottom) v1 -= (v1 - v0) * (y1 - r.bottom) / (y1 - y0), y1 = r.bottom;
      if (x1 > x0 && y1 > y0) quad(x0, y0, x1, y1, k, k, u0, v0, u1, v1, nullptr, 1.0f);
    }
    pen += g.adv * scale;
  }
}

// ---------------------------------------------------------------------------
Video::Video() = default;
Video::~Video() { shutdown(); }

bool Video::init(SDL_Window* window) {
  shutdown();
  window_ = window;
  gl_ = SDL_GL_CreateContext(window);
  if (!gl_) {
    logError(SDL_GetError());
    return false;
  }
  SDL_GL_MakeCurrent(window, (SDL_GLContext)gl_);
  SDL_GL_SetSwapInterval(1);
  SDL_GL_GetDrawableSize(window, &w_, &h_);
#ifdef __SWITCH__
  {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);
    const char* vendor = (const char*)glGetString(GL_VENDOR);
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    const char* version = (const char*)glGetString(GL_VERSION);
    const char* glsl = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
    logInfo("GL vendor \"%s\", renderer \"%s\"", vendor ? vendor : "?", renderer ? renderer : "?");
    logInfo("GL version \"%s\", GLSL \"%s\"", version ? version : "?", glsl ? glsl : "?");
    logInfo("window %dx%d, drawable %dx%d", ww, wh, w_, h_);
    if (w_ <= 0 || h_ <= 0) {
      // (SDL gave no drawable size: the window's, else the console's own display size: 0x0 would
      // leave the viewport empty and the screen black)
      w_ = ww;
      h_ = wh;
      if (w_ <= 0 || h_ <= 0) nx::displaySize(&w_, &h_);
      logInfo("no drawable size from SDL: using %dx%d", w_, h_);
    }
    checkGl("context setup");
  }
  const Uint32 t0 = SDL_GetTicks();
#endif
  liteShader_ = false;
#ifdef SMAS_TEST_FORCE_LITE
  prog_ = 0;  // (test builds: as if the full picture shader did not build)
#else
  prog_ = linkProgram(kGameVS, kGameFS);
#endif
  if (!prog_) {
    // (the full picture shader did not build on this driver: the plain one, without the CRT, LCD,
    // colour-vision and blurred-backdrop passes, still shows the game)
    logError("the picture shader did not build: trying the plain one");
    std::string lite = kGameFS;
    lite.insert(lite.find('\n') + 1, "#define SMAS_LITE\n");
    prog_ = linkProgram(kGameVS, lite);
    liteShader_ = prog_ != 0;
  }
#ifdef __SWITCH__
  logInfo("picture shader %s in %u ms%s", prog_ ? "built" : "FAILED", (unsigned)(SDL_GetTicks() - t0),
          liteShader_ ? " (plain version)" : "");
  checkGl("picture shader");
#endif
  if (!prog_) return false;
  const char* names[] = {"u_texP", "u_texL", "u_texSize", "u_outSize", "u_f0", "u_f1", "u_f2", "u_f3"};
  for (int i = 0; i < 8; i++) {
    loc_[i] = glGetUniformLocation(prog_, names[i]);
#ifdef __SWITCH__
    if (loc_[i] < 0) logInfo("picture shader: uniform %s not found", names[i]);
#endif
  }
  glGenVertexArrays(1, &vao_);
  glGenSamplers(2, samplers_);
  for (int i = 0; i < 2; i++) {
    GLint f = i ? GL_LINEAR : GL_NEAREST;
    glSamplerParameteri(samplers_[i], GL_TEXTURE_MIN_FILTER, f);
    glSamplerParameteri(samplers_[i], GL_TEXTURE_MAG_FILTER, f);
    glSamplerParameteri(samplers_[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(samplers_[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  static unsigned epochs = 0;
  canvas_ = std::make_unique<GlesCanvas>(++epochs);
#ifdef __SWITCH__
  const Uint32 t1 = SDL_GetTicks();
  const bool canvasOk = canvas_->create();
  logInfo("UI shader %s in %u ms", canvasOk ? "built" : "FAILED", (unsigned)(SDL_GetTicks() - t1));
  checkGl("UI setup");
  if (!canvasOk) return false;
#else
  if (!canvas_->create()) return false;
#endif
  return true;
}

void Video::shutdown() {
  if (gl_) {
    canvas_.reset();
    if (tex_) glDeleteTextures(1, &tex_);
    if (prog_) glDeleteProgram(prog_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    if (samplers_[0]) glDeleteSamplers(2, samplers_);
    SDL_GL_DeleteContext((SDL_GLContext)gl_);
  }
  canvas_.reset();
  tex_ = prog_ = vao_ = samplers_[0] = samplers_[1] = 0;
  texW_ = texH_ = 0;
  gl_ = nullptr;
  window_ = nullptr;
}

void Video::resize(int w, int h) {
  if (w > 0 && h > 0) w_ = w, h_ = h;
}

Canvas* Video::canvas() { return canvas_.get(); }

void Video::upload(const uint32_t* px, int w, int h, int stride) {
  if (!gl_ || !px || w <= 0 || h <= 0) return;
  if (!tex_) glGenTextures(1, &tex_);
  glBindTexture(GL_TEXTURE_2D, tex_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
  if (w != texW_ || h != texH_) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    texW_ = w;
    texH_ = h;
  } else {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
  }
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void Video::clear() {
  if (!gl_) return;
  glViewport(0, 0, w_, h_);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
}

void Video::pass(const VideoParams& p, int which, int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;  // (a negative viewport is a GL error)
  glViewport(x, h_ - (y + h), w, h);
  glUseProgram(prog_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex_);
  glBindSampler(0, samplers_[0]);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, tex_);
  glBindSampler(1, samplers_[1]);
  glUniform1i(loc_[0], 0);
  glUniform1i(loc_[1], 1);
  glUniform4f(loc_[2], (float)texW_, (float)texH_, 1.0f / texW_, 1.0f / texH_);
  glUniform4f(loc_[3], (float)w, (float)h, 1.0f / std::max(w, 1), 1.0f / std::max(h, 1));
  glUniform4f(loc_[4], (float)p.filter, p.scanlines, p.curve, (float)which);
  glUniform4f(loc_[5], p.brightness, p.contrast, p.saturation, p.gamma);
  glUniform4f(loc_[6], (float)p.colorProfile, (float)p.colorblind, (float)p.srcW, (float)p.srcH);
  glUniform4f(loc_[7], p.fade, (float)p.pillarbox, p.temperature, 0);
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
  glBindSampler(0, 0);
  glBindSampler(1, 0);
  glActiveTexture(GL_TEXTURE0);
}

void Video::draw(const VideoParams& p) {
  clear();
  if (!tex_ || !prog_) return;
  glDisable(GL_BLEND);
  bool fullCover = p.dest.left <= 0 && p.dest.top <= 0 && p.dest.right >= w_ && p.dest.bottom >= h_;
  if (p.pillarbox == 1 && !fullCover) pass(p, 1, 0, 0, w_, h_);
  pass(p, 0, p.dest.left, p.dest.top, p.dest.right - p.dest.left, p.dest.bottom - p.dest.top);
  glViewport(0, 0, w_, h_);
}

void Video::beginUi() {
  if (!canvas_) return;
  glViewport(0, 0, w_, h_);
  canvas_->begin(w_, h_);
}

void Video::endUi() {
  if (canvas_) canvas_->end();
  glDisable(GL_SCISSOR_TEST);
}

void Video::present() {
  if (window_) SDL_GL_SwapWindow(window_);
#ifdef __SWITCH__
  // (the first frames reported, so the log shows whether the loop runs and the driver is content)
  static unsigned frames = 0;
  ++frames;
  if (frames <= 3 || frames == 60 || frames == 600) {
    logInfo("frame %u presented (%dx%d)", frames, w_, h_);
    checkGl("a frame");
  }
#endif
}

bool Video::capture(std::vector<uint32_t>& out, int* w, int* h) {
  if (!gl_) return false;
  std::vector<uint8_t> px((size_t)w_ * h_ * 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  out.resize((size_t)w_ * h_);
  for (int y = 0; y < h_; y++) {
    const uint8_t* s = &px[(size_t)(h_ - 1 - y) * w_ * 4];
    for (int x = 0; x < w_; x++, s += 4) out[(size_t)y * w_ + x] = (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
  }
  *w = w_;
  *h = h_;
  return true;
}
