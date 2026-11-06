// Video on Android: OpenGL ES 3.0. The game picture with the same filters and
// colour settings as the Direct3D 11 one (app/video.cpp), then the UI drawn on
// a Canvas (canvas.h) whose text comes from Android's own font engine
// (SmasActivity.renderGlyphs, through JNI).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "app/canvas.h"

struct VideoParams {
  int srcW = 256, srcH = 224;  // active source size (pixels)
  IRect dest{};                // destination rect in screen pixels
  int filter = 1;
  float scanlines = 0.45f;
  float curve = 0.0f;
  int colorProfile = 0;
  float brightness = 1, contrast = 1, saturation = 1, gamma = 1;
  int colorblind = 0;
  float temperature = 0;  // -1 cool .. +1 warm
  int pillarbox = 1;      // 0 black, 1 blurred, 2 dark frame
  float fade = 1.0f;
  unsigned frame = 0;
};

struct SDL_Window;
class GlesCanvas;

class Video {
public:
  Video();
  ~Video();
  bool init(SDL_Window* window);
  void shutdown();
  void resize(int w, int h);
  int width() const { return w_; }
  int height() const { return h_; }

  void upload(const uint32_t* px, int w, int h, int stride);
  void draw(const VideoParams& p);
  void clear();  // black, no game picture (the launcher)
  Canvas* canvas();
  void beginUi();
  void endUi();
  void present();
  bool capture(std::vector<uint32_t>& out, int* w, int* h);

private:
  SDL_Window* window_ = nullptr;
  void* gl_ = nullptr;
  int w_ = 0, h_ = 0;
  unsigned prog_ = 0, vao_ = 0, tex_ = 0, samplers_[2] = {};
  int texW_ = 0, texH_ = 0;
  int loc_[12] = {};
  bool liteShader_ = false;  // (the plain picture shader is in use: see Video::init)
  std::unique_ptr<GlesCanvas> canvas_;
  void pass(const VideoParams& p, int pass, int x, int y, int w, int h);
};
