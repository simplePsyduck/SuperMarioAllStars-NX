#include "app/launcher.h"
#include "app/imageload.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "app/audio.h"
#include "app/logo_data.h"
#include "core/snes.h"

namespace {
const char* kButtonNames[Launcher::kButtons] = {"Options", "Achievements", "Music Player", "Credits", "Quit"};

struct CardInfo {
  GameId game;
  const char* title;
  const char* subtitle;
  const char* year;
  int players;
};
const CardInfo kCardInfo[Launcher::kCards + 1] = {
    {GameId::SMB1, "Super Mario Bros.", "Starring Mario & Luigi", "1985", 2},
    {GameId::SMBLL, "The Lost Levels", "For Super Players", "1986", 1},
    {GameId::SMB2, "Super Mario Bros. 2", "Starring Mario, Luigi, Toad & Peach", "1988", 1},
    {GameId::SMB3, "Super Mario Bros. 3", "Starring Mario & Luigi", "1988", 2},
    {GameId::Menu, "Super Mario All-Stars", "Four Super Mario classics on one cartridge", "1993", 2},
};

ColorF argb(uint32_t c) {
  return colorF(((c >> 16) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f, (c & 0xff) / 255.0f,
                      ((c >> 24) & 0xff) / 255.0f);
}
ColorF withA(ColorF c, float a) { return colorF(c.r, c.g, c.b, c.a * a); }
ColorF lerp(ColorF x, ColorF y, float t) {
  return colorF(x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t);
}
constexpr float kLine = 1.33f;  // Segoe UI line height per pixel of font size
}  // namespace

Launcher::~Launcher() { releaseGpu(); }

GameId Launcher::cardGame(int i) { return kCardInfo[std::clamp(i, 0, (int)kCards)].game; }

ColorF Launcher::gameAccent(GameId g) {
  switch (g) {
    case GameId::SMB1: return colorF(0.93f, 0.33f, 0.27f);   // Mario red
    case GameId::SMBLL: return colorF(0.36f, 0.76f, 0.40f);  // Luigi green
    case GameId::SMB2: return colorF(0.66f, 0.52f, 0.95f);   // dream purple
    case GameId::SMB3: return colorF(0.96f, 0.62f, 0.20f);   // SMB3 orange
    default: return colorF(0.91f, 0.72f, 0.29f);             // All-Stars gold
  }
}

// The menu's colour over the launcher: the chosen card's game, gold on the buttons.
ColorF Launcher::accent() const {
  return sel_ < (int)cards_.size() ? gameAccent(cardGame(cards_[sel_])) : gameAccent(GameId::Menu);
}

void Launcher::selectGame(GameId g) {
  for (int i = 0; i < (int)cards_.size(); i++)
    if (kCardInfo[cards_[i]].game == g) sel_ = lastCard_ = i;
}

// One card, the All-Stars cartridge as it is (its title and game select), or
// the four games'. Previews start as their cards are first shown.
void Launcher::setAllStarsOnly(bool on) {
  std::vector<int> c = on ? std::vector<int>{kAllStars} : std::vector<int>{0, 1, 2, 3};
  if (c == cards_) return;
  GameId was = cardGame(card());
  bool onButton = sel_ >= (int)cards_.size();
  int button = sel_ - (int)cards_.size();
  cards_ = c;
  sel_ = lastCard_ = 0;
  selectGame(was);
  if (onButton) sel_ = (int)cards_.size() + button;
  for (float& a : anim_) a = 0;
  soundCard_ = -1;
}

void Launcher::releaseGpu() {
  if (canvas_ && canvas_->epoch() == epoch_) {
    if (floor_) canvas_->freeShape(floor_);
    if (star_) canvas_->freeShape(star_);
    for (Preview& p : prev_)
      if (p.image) canvas_->freeImage(p.image);
    if (logo_) canvas_->freeImage(logo_);
    for (Art& a : cover_)
      if (a.image) canvas_->freeImage(a.image);
    if (bgArt_.image) canvas_->freeImage(bgArt_.image);
  }
  floor_ = star_ = logo_ = 0;
  for (Preview& p : prev_) p.image = 0;
  for (Art& a : cover_) a.image = 0;
  bgArt_.image = 0;
  canvas_ = nullptr;
}

const char* Launcher::artName(int card) {
  static const char* const kNames[kCards + 1] = {"smb1", "lostlevels", "smb2", "smb3", "allstars"};
  return card >= 0 && card <= kCards ? kNames[card] : "background";
}

void Launcher::freeArt() {
  for (Art* a : {&cover_[0], &cover_[1], &cover_[2], &cover_[3], &cover_[4], &bgArt_}) {
    if (a->image && canvas_ && canvas_->epoch() == epoch_) canvas_->freeImage(a->image);
    *a = Art{};
  }
}

void Launcher::readArt(Art& a, const char* name) {
  for (const wchar_t* ext : {L".png", L".jpg", L".jpeg", L".PNG", L".JPG", L".JPEG"}) {
    std::wstring path = artDir_ + L"/" + std::wstring(name, name + strlen(name)) + ext;
    // (a big picture shrunk: 1024 px is plenty for a card, 2048 for a background)
    if (loadImageFile(path, &a == &bgArt_ ? 2048 : 1024, a.px, &a.w, &a.h)) return;
  }
  a = Art{};
}

void Launcher::setArt(const std::wstring& dir, bool covers, bool background, bool reload) {
  if (!reload && dir == artDir_ && covers == useCovers_ && background == useBg_) return;
  freeArt();
  artDir_ = dir;
  useCovers_ = covers;
  useBg_ = background;
  if (covers)
    for (int i = 0; i <= kCards; i++) readArt(cover_[i], artName(i));
  if (background) readArt(bgArt_, "background");
  // (a card that has a cover now: its preview stopped, its sound with it)
  for (int i = 0; i <= kCards; i++)
    if (hasCover(i)) {
      prev_[i].game.reset();
      prev_[i].snes.reset();
      prev_[i].ready = false;
      prev_[i].bootFrames = 0;
      prev_[i].shown = 0;
      if (soundCard_ == i) soundCard_ = -1;
    }
}

int Launcher::artImage(Canvas* cv, Art& a) {
  if (!a.image && a.w > 0) {
    a.image = cv->makeImage(a.w, a.h, true);
    if (a.image) cv->updateImage(a.image, a.px.data(), a.w);
  }
  return a.image;
}

// (A new canvas (the device rebuilt): what was made on the old one is gone.)
void Launcher::useCanvas(Canvas* cv) {
  if (cv == canvas_ && cv && cv->epoch() == epoch_) return;
  if (canvas_ != cv || !cv || cv->epoch() != epoch_) {
    floor_ = star_ = logo_ = 0;
    for (Preview& p : prev_) p.image = 0;
    for (Art& a : cover_) a.image = 0;
    bgArt_.image = 0;
  }
  canvas_ = cv;
  epoch_ = cv ? cv->epoch() : 0;
}

void Launcher::setRom(const std::vector<uint8_t>* rom, const std::string& fileName, bool exact, const uint8_t* sram,
                      size_t sramSize) {
  haveRom_ = rom != nullptr;
  romName_ = fileName;
  exact_ = exact;
  rom_ = rom;
  sram_.assign(sram, sram + (rom ? sramSize : 0));
  for (Preview& p : prev_) {
    p.game.reset();
    p.snes.reset();
    p.ready = false;
    p.bootFrames = 0;
    p.shown = 0;
  }
}

void Launcher::startPreview(int i) {
  Preview& p = prev_[i];
  p.snes = std::make_unique<Snes>();
  if (!p.snes->loadRom(*rom_)) {
    p.snes.reset();
    return;
  }
  p.game = std::make_unique<SmasGame>(*p.snes);
  p.game->install();
  p.snes->reset(true);
  std::memcpy(p.snes->sram, sram_.data(), std::min(sram_.size(), sizeof(p.snes->sram)));
  p.game->setWidescreen(0);
  p.game->hudAnchor = false;
  p.game->achievements().newRun(false);
  // (the All-Stars card: the cartridge from power-on, its title and game select)
  if (i != kAllStars) p.game->requestTitle(cardGame(i), true);
}

void Launcher::stepPreview(Preview& p) {
  for (int k = 0; k < 2; k++) p.snes->input[k] = 0;
  p.snes->apu.dsp.musicGain = musicGain_;
  p.snes->apu.dsp.sfxGain = sfxGain_;
  p.game->beforeFrame();
  p.snes->runFrame();
  p.game->afterFrame();
  buf_.clear();
  p.snes->takeAudio(buf_);
}

void Launcher::runPreviews(int frames, bool sound, Audio& audio, double refreshHz) {
  // Starting: through the game select out of sight, the chosen card's first,
  // a few milliseconds of it per refresh.
  using clock = std::chrono::steady_clock;
  auto start = clock::now();
  const int N = (int)cards_.size();
  for (int k = 0; k < N; k++) {
    int i = cards_[(lastCard_ + k) % N];
    Preview& p = prev_[i];
    if (hasCover(i)) continue;  // (its cover instead)
    if (!p.snes && rom_) startPreview(i);
    if (!p.snes || p.ready) continue;
    // (Booting out of sight, nothing drawn: the Steam Deck, at 60 Hz and a
    // slower CPU, took half a minute to bring all the previews up, SMB3's,
    // the last, still "loading" long after the others ran.)
    p.snes->ppu.skipRender = true;
    while (!p.ready && clock::now() - start < std::chrono::microseconds(6000)) {
      stepPreview(p);
      p.bootFrames++;
      p.ready = i == kAllStars ? p.bootFrames > 30
                               : (!p.game->levelLoading() && p.game->current() == cardGame(i)) || p.bootFrames > 6000;
    }
    p.snes->ppu.skipRender = !p.ready;
  }
  int want = sound && sel_ < N && prev_[cards_[sel_]].ready ? cards_[sel_] : -1;
  if (want != soundCard_) {
    audio.flush();
    soundCard_ = want;
  }
  // Four consoles drawing every frame made the launcher slow (144 Hz
  // displays, phones). Drawing a picture is most of a frame's work: the
  // chosen card runs and draws every frame, with its sound; the others run
  // their frames (only the last of them drawn, and only one card's picture
  // a frame, in turn: small on screen, a few times a second is plenty)
  // within a share of the refresh's time. On a fast computer they keep up;
  // on a slow device they fall behind a little rather than the launcher.
  const int chosen = N > 0 ? cards_[(sel_ < N ? sel_ : lastCard_) % N] : -1;
  int others[kCards + 1], nOthers = 0;
  for (int i : cards_)
    if (i != chosen && prev_[i].snes && prev_[i].ready) others[nOthers++] = i;
  const int turn = frames > 0 && nOthers > 0 ? others[drawTurn_ % nOthers] : -1;
  if (frames > 0) drawTurn_++;
  if (chosen >= 0 && prev_[chosen].snes && prev_[chosen].ready) {
    Preview& p = prev_[chosen];
    p.owed = 0;
    for (int f = 0; f < frames; f++) {
      const bool draw = f == frames - 1;
      p.snes->ppu.skipRender = !draw;
      stepPreview(p);
      if (draw) p.fresh = true;
      if (chosen == soundCard_ && !buf_.empty()) {
        for (int16_t& s : buf_) s = (int16_t)(s * 0.55f);  // (quieter than the game)
        audio.push(buf_.data(), buf_.size() / 2, Snes::kSampleRate);
      }
    }
  }
  const auto budget = std::chrono::microseconds(refreshHz > 0 ? (int)(0.35e6 / refreshHz) : 1 << 30);
  const auto from = clock::now();
  for (int n = 0; n < nOthers; n++) {
    Preview& p = prev_[others[n]];
    p.owed = std::min(p.owed + frames, 3);  // (fallen further behind: it just runs slower)
  }
  for (int n = 0; n < nOthers; n++) {
    const int i = others[(drawTurn_ + n) % nOthers];
    Preview& p = prev_[i];
    while (p.owed > 0 && clock::now() - from < budget) {
      const bool draw = p.owed == 1 && i == turn;
      p.snes->ppu.skipRender = !draw;
      stepPreview(p);
      p.owed--;
      if (draw) p.fresh = true;
    }
  }
}

Launcher::Action Launcher::update(const MenuInput& in, float dt) {
  time_ += dt;
  const int N = (int)cards_.size();  // cards 0..N-1, buttons N..N+4
  // A held direction moves once, then repeats, slower than the menus (moved
  // every refresh, a pad's stick flew along the cards).
  int dir = in.up ? 1 : in.down ? 2 : in.left ? 3 : in.right ? 4 : 0;
  bool fire = false;
  if (dir != repeatDir_) {
    repeatDir_ = dir;
    repeatTimer_ = 0.45f;
    fire = dir != 0;
  } else if (dir != 0 && (repeatTimer_ -= dt) <= 0) {
    repeatTimer_ = 0.2f;
    fire = true;
  }
  int& s = sel_;
  if (fire && dir == 3 && s != 0 && s != N) s--;
  if (fire && dir == 4 && s != N - 1 && s != N + kButtons - 1) s++;
  if (fire && dir == 2 && s < N) s = N + 2;
  if (fire && dir == 1 && s >= N) s = lastCard_;
  if (s < N) lastCard_ = s;
  for (int i = 0; i < N + kButtons; i++) {
    float target = i == s ? 1.0f : 0.0f;
    anim_[i] += (target - anim_[i]) * std::min(1.0f, dt * 12.0f);
  }
  for (Preview& p : prev_)
    if (p.ready) p.shown = std::min(1.0f, p.shown + dt * 3.0f);
  if (in.back) return kQuit;
  if (!in.confirm) return kNone;
  if (s < N) return kPlay;
  static const Action kActs[kButtons] = {kOptions, kAchievements, kMusic, kCredits, kQuit};
  return kActs[s - N];
}

// The background, after the All-Stars title screen: a night-blue sky with
// twinkling gold stars over a blue checkerboard floor running to the horizon
// (drawn here: no picture from the game).
void Launcher::drawBackground(Menu& menu, float W, float H) {
  Canvas* cv = menu.canvas();
  if (useBg_ && bgArt_.w > 0) {
    // The player's picture: filling the window (its middle, the rest cut
    // off; not stretched), darkened a little and shaded at the bottom, so
    // the title, cards and buttons read over anything.
    if (int img = artImage(cv, bgArt_)) {
      const float s = std::max(W / bgArt_.w, H / bgArt_.h), dw = bgArt_.w * s, dh = bgArt_.h * s;
      cv->fillRect(rectF(0, 0, W, H), argb(0xFF000000));
      cv->pushClip(rectF(0, 0, W, H));
      cv->drawImage(img, rectF((W - dw) * 0.5f, (H - dh) * 0.5f, (W + dw) * 0.5f, (H + dh) * 0.5f), 1.0f, true);
      cv->popClip();
      cv->fillRect(rectF(0, 0, W, H), colorF(0.0f, 0.0f, 0.03f, 0.35f));
      const float shade = H * 0.25f;
      for (float y = H - shade; y < H; y += 2) {
        float a = (y - (H - shade)) / shade;
        cv->fillRect(rectF(0, y, W, y + 2), colorF(0.0f, 0.0f, 0.03f, 0.75f * a * a));
      }
      // (and at the top, behind the logo, the title and the line under it)
      const float top = H * 0.24f;
      for (float y = 0; y < top; y += 2) {
        float a = 1.0f - y / top;
        cv->fillRect(rectF(0, y, W, y + 2), colorF(0.0f, 0.0f, 0.03f, 0.7f * a * (2.0f - a) * 0.75f));
      }
      return;
    }
  }
  const float horizon = std::round(H * 0.6f);
  const ColorF skyTop = argb(0xFF070F2C), skyLow = argb(0xFF1D3B88), floorDark = argb(0xFF132A63),
               floorLight = argb(0xFF3459B2);
  if (bgW_ != W || bgH_ != H) {  // (the floor fits the window)
    releaseBackground();
    bgW_ = W;
    bgH_ = H;
  }
  if (!floor_) {
    // Square tiles in perspective: a row at depth z (1 at the bottom edge)
    // is at y = horizon + (H - horizon) / z, a column edge X at
    // x = W/2 + X * tile / z; the light tiles, one figure each.
    std::vector<PointF> pts;
    std::vector<int> counts;
    const float k = H - horizon, tile = H / 7.0f, dz = tile / k;
    for (int row = 0; row < 200; row++) {
      float z0 = 1 + row * dz, z1 = z0 + dz;
      float y0 = horizon + k / z0, y1 = horizon + k / z1;
      if (y0 - y1 < 0.75f) break;
      int cols = (int)std::ceil(W * 0.5f * z1 / tile) + 1;
      for (int c = -cols; c < cols; c++) {
        if (((c + row) & 1) == 0) continue;
        pts.push_back({W * 0.5f + c * tile / z0, y0});
        pts.push_back({W * 0.5f + (c + 1) * tile / z0, y0});
        pts.push_back({W * 0.5f + (c + 1) * tile / z1, y1});
        pts.push_back({W * 0.5f + c * tile / z1, y1});
        counts.push_back(4);
      }
    }
    floor_ = cv->makeShape(pts, counts);
  }
  if (!star_) {
    std::vector<PointF> pts(10);
    for (int i = 0; i < 10; i++) {
      float r = (i & 1) ? 0.45f : 1.0f, a = -1.5707963f + i * 0.6283185f;
      pts[i] = {r * std::cos(a), r * std::sin(a)};
    }
    star_ = cv->makeShape(pts, {10});
  }

  cv->fillGradient(rectF(0, 0, W, horizon), skyTop, skyLow);
  cv->fillRect(rectF(0, horizon, W, H), floorDark);
  if (floor_) cv->fillShape(floor_, floorLight);
  // (the floor fades into the haze at the horizon)
  const float fogEnd = horizon + (H - horizon) * 0.7f;
  cv->fillGradient(rectF(0, horizon, W, fogEnd), skyLow, withA(skyLow, 0.0f));
  // (shaded towards the bottom edge: the buttons and the hint stand clear of the tiles)
  const float shade = H * 0.2f;
  for (float y = H - shade; y < H; y += 2) {
    float a = (y - (H - shade)) / shade;
    cv->fillRect(rectF(0, y, W, y + 2), colorF(0.02f, 0.04f, 0.10f, 0.7f * a * a));
  }

  // Stars: fixed places across the sky (the same at any size), each
  // twinkling at its own pace.
  if (star_) {
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    uint32_t seed = 12345;
    auto rnd = [&]() {
      seed = seed * 1664525u + 1013904223u;
      return (seed >> 8) / 16777216.0f;
    };
    for (int i = 0; i < 34; i++) {
      float x = rnd() * W, y = rnd() * (horizon - H * 0.03f), size = H * (0.006f + rnd() * 0.012f);
      float speed = 0.6f + rnd() * 1.4f, phase = rnd() * 6.283f;
      if (std::fabs(x - W * 0.5f) < std::min(W * 0.36f, H * 0.62f) && y < H * 0.23f) continue;  // (clear of the title)
      float a = 0.15f + 0.3f * (0.5f + 0.5f * std::sin((float)t * speed + phase));
      cv->fillShape(star_, colorF(1.0f, 0.82f, 0.30f, a), size, x, y);
    }
  }
}

void Launcher::releaseBackground() {
  if (floor_ && canvas_ && canvas_->epoch() == epoch_) canvas_->freeShape(floor_);
  floor_ = 0;
}

void Launcher::draw(Menu& menu, int w, int h) {
  Canvas* cv = menu.canvas();
  if (!cv) return;
  useCanvas(cv);
  const float W = (float)w, H = (float)h;
  const int N = (int)cards_.size();
  // (The Mickey & Donald Collection's fonts: a share of the height, or of
  // three quarters of the width on narrow windows.)
  const float base = std::min(H, W * 0.75f);
  const float fSmall = std::max(8.0f, std::round(base / 40)), fBody = std::max(8.0f, std::round(base / 28)),
              fTitle = std::max(8.0f, std::round(base / 16)), fHuge = std::max(8.0f, std::round(base / 9));
  const auto lh = [](float size, float scale = 1.0f) { return size * scale * kLine; };
  // (the canvas's own text: the menus' style is for the menus and the buttons below)
  const auto measure = [&](const std::string& t, float size, bool bold) {
    return t.empty() ? 0.0f : cv->measure(Menu::widen(t), size, bold ? kWeightBold : kWeightNormal);
  };
  // text from its top edge: left, centred on x, or right
  // (over the player's own background picture, text gets a dark outline: it reads over anything)
  const bool customBg = useBg_ && bgArt_.w > 0;
  const auto text = [&](float x, float y, const std::string& t, float size, bool bold, ColorF c, int align,
                        float scale = 1.0f) {
    float sz = size * scale, l = lh(size, scale);
    RectF r = align == 0   ? rectF(x, y, x + 20000, y + l)
                    : align == 1 ? rectF(x - 10000, y, x + 10000, y + l)
                                 : rectF(x - 20000, y, x, y + l);
    if (customBg) {
      const std::wstring ws = Menu::widen(t);
      const float o = std::max(1.0f, sz * 0.07f);
      const TextAlign al = align == 0 ? kAlignLeading : align == 1 ? kAlignCenter : kAlignTrailing;
      for (int k = 0; k < 8; k++) {
        const float ang = k * 3.14159265f / 4;
        cv->text(ws, rectF(r.left + std::cos(ang) * o, r.top + std::sin(ang) * o, r.right + std::cos(ang) * o,
                           r.bottom + std::sin(ang) * o),
                 sz, bold ? kWeightBold : kWeightNormal, colorF(0, 0, 0.02f, 0.8f * c.a), al, kParaNear);
      }
      // (the grey lines lighter, the outline doing the separating)
      if (c.r < 0.75f && std::fabs(c.r - c.b) < 0.2f && std::fabs(c.g - c.b) < 0.2f)
        c = colorF(c.r + (1 - c.r) * 0.55f, c.g + (1 - c.g) * 0.55f, c.b + (1 - c.b) * 0.55f, c.a);
    }
    cv->text(Menu::widen(t), r, sz, bold ? kWeightBold : kWeightNormal, c,
             align == 0   ? kAlignLeading
             : align == 1 ? kAlignCenter
                          : kAlignTrailing,
             kParaNear);
  };
  const auto fill = [&](RectF r, ColorF c) { cv->fillRect(r, c); };
  const ColorF acc = accent();

  drawBackground(menu, W, H);

  // Title
  // Title, in the title font and the Mario logo's way: each letter of "Super
  // Mario" a colour of its own in turn (red, yellow, green, blue), "All-Stars"
  // gold, every letter outlined dark over a drop shadow.
  const std::wstring kTitle = L"SUPER MARIO ALL-STARS";
  float ty0 = H * 0.035f;
  float tsize = fHuge * 0.8f;
  float tw = cv->measureTitle(kTitle, tsize);
  // (room beside it for the logo, top left: narrower, the title gives some)
  const float logoM = H * 0.03f, titleMax = std::min(W * 0.86f, W - 2 * (H * 0.16f + logoM * 2));
  if (tw > titleMax) {
    tsize *= titleMax / tw;
    tw = titleMax;
  }
  {
    const float th = lh(tsize), o = std::max(1.5f, tsize * 0.05f);
    const ColorF kLetters[4] = {argb(0xFFE52521), argb(0xFFFBD000), argb(0xFF43B047), argb(0xFF049CD8)};
    const ColorF outline = argb(0xFF0B1230), gold = argb(0xFFFFC425);
    float x = (W - tw) * 0.5f;
    int k = 0;
    for (size_t i = 0; i < kTitle.size(); i++) {
      const std::wstring ch(1, kTitle[i]);
      const float chw = cv->measureTitle(ch, tsize);
      if (kTitle[i] != L' ') {
        // (a box wider than the letter, centred on it: the outline isn't clipped)
        auto at = [&](float dx, float dy) { return rectF(x - o * 3 + dx, ty0 + dy, x + chw + o * 3 + dx, ty0 + th + dy); };
        cv->titleText(ch, at(o * 0.9f, o * 1.8f), tsize, withA(outline, 0.55f), kAlignCenter);
        for (int d = 0; d < 8; d++) {
          float a = d * 0.7853982f;
          cv->titleText(ch, at(std::cos(a) * o, std::sin(a) * o), tsize, outline, kAlignCenter);
        }
        cv->titleText(ch, at(0, 0), tsize, i < 11 ? kLetters[k++ % 4] : gold, kAlignCenter);
      }
      x += chw;
    }
    ty0 += th;
  }
#if defined(__SWITCH__)
  const char* kEdition = "Switch Edition";
#elif !defined(__ANDROID__)
  const char* kEdition = "PC Edition";
#else
  const char* kEdition = "Android Edition";
#endif
  // The Team RedTail logo, top left, the Patreon page under it: smaller
  // where the title leaves less room beside it (none: not shown).
  {
    if (!logo_) {
      logo_ = cv->makeImage(kLogoW, kLogoH, true);
      if (logo_) {
        std::vector<uint32_t> px;
        px.reserve((size_t)kLogoW * kLogoH);
        for (size_t k = 0; k + 1 < sizeof(kLogoRle); k += 2) px.insert(px.end(), kLogoRle[k], kLogoPal[kLogoRle[k + 1]]);
        px.resize((size_t)kLogoW * kLogoH);
        cv->updateImage(logo_, px.data(), kLogoW);
      }
    }
    const float m = logoM, room = (W - tw) * 0.5f - m * 2;
    const float lw = std::min(room, H * 0.13f * kLogoW / kLogoH), lhgt = lw * kLogoH / kLogoW;
    linkHit_ = RectF{};
    if (logo_ && lw > H * 0.06f) {
      const std::string kPage = "patreon.com/TeamRedTail";
      const float fs = std::min(fSmall, fSmall * std::min(lw * 1.7f, room) / std::max(1.0f, measure(kPage, fSmall, false)));
      const float bw = std::max(lw, measure(kPage, fs, false));  // (the two centred on each other)
      cv->drawImage(logo_, rectF(m + (bw - lw) * 0.5f, m, m + (bw + lw) * 0.5f, m + lhgt), 1.0f, true);
      text(m + bw * 0.5f, m + lhgt + H * 0.004f, kPage, fs, false, argb(0xFF9AA4BA), 1);
      linkHit_ = rectF(m, m, m + bw, m + lhgt + H * 0.004f + lh(fs));
    }
  }
  // A newer release: a badge, top right (it opens its page)
  updateHit_ = RectF{};
  if (!updateTag_.empty()) {
    // (beside the title if there's room; else, shorter, under the Patreon link)
    const std::string ver = updateTag_.size() > 1 && (updateTag_[0] == 'v' || updateTag_[0] == 'V') ? updateTag_.substr(1)
                                                                                                    : updateTag_;
    std::string t = "Update available: v" + ver;
    const float fs = fSmall, bh = lh(fs) * 1.45f, m = logoM;
    float bw = measure(t, fs, true) + bh * 1.7f;
    RectF r;
    if (W - m - bw >= (W + tw) * 0.5f + m) {
      r = rectF(W - m - bw, H * 0.075f, W - m, H * 0.075f + bh);
    } else {
      t = "v" + ver + " available";
      bw = measure(t, fs, true) + bh * 1.7f;
      const float top = linkHit_.bottom > linkHit_.top ? linkHit_.bottom + H * 0.012f : m;
      r = rectF(m, top, m + bw, top + bh);
    }
    const float pulse = 0.5f + 0.5f * std::sin(time_ * 3.0f);
    menu.fillRound(rectF(r.left - 3, r.top - 3, r.right + 3, r.bottom + 3), bh * 0.5f + 3,
                   colorF(1.0f, 0.85f, 0.25f, 0.25f + 0.25f * pulse));
    menu.fillRound(r, bh * 0.5f, argb(0xFFFFD400));
    menu.outlineRound(r, bh * 0.5f, std::max(2.0f, H / 360.0f), argb(0xFF3A1D0E));
    menu.star(r.left + bh * 0.55f, (r.top + r.bottom) * 0.5f, bh * 0.28f, argb(0xFFFFFFFF), argb(0xFF3A1D0E),
              std::max(1.0f, H / 700.0f));
    cv->text(Menu::widen(t), rectF(r.left + bh * 1.0f, r.top, r.right - bh * 0.5f, r.bottom), fs, kWeightBold,
             argb(0xFF3A1D0E), kAlignCenter, kParaCenter);
    updateHit_ = r;
  }
  text(W * 0.5f, ty0, std::string("Super Nintendo classics  \xE2\x80\xA2  ") + kEdition + "  \xE2\x80\xA2  v" + version_, fBody, false,
       argb(0xFF9AA4BA), 1);
  ty0 += lh(fBody);

  // Cards
  // (the cards' picture frames: the game's screen's shape; with covers, square:
  // upright box art, the frames fill better)
  const float screenAspect = (float)(256.0 * par_ / 224.0);
  bool anyCover = false;
  for (int k : cards_) anyCover = anyCover || hasCover(k);
  const float aspect = anyCover ? 1.0f : screenAspect;
  const float top = ty0 + H * 0.045f;
  const float gap = W * 0.025f;
  const float rad0 = H * 0.02f;
  const float infoH = rad0 * 0.7f + lh(fTitle) * 0.95f + lh(fSmall) * (1.05f + 1.1f + 1.0f) + rad0 * 0.6f;
  const float cw = std::min((W * 0.95f - gap * (N - 1)) / N, (H * 0.82f - top - infoH - rad0 * 0.6f) * aspect + rad0 * 1.2f);
  const float ph = rad0 * 0.6f + (cw - rad0 * 1.2f) / aspect;
  const float ch = ph + infoH;
  const float x0 = (W - (cw * N + gap * (N - 1))) * 0.5f;
  // (Four cards are less tall than the room: centred in it, above the buttons.)
  const float by = H * 0.855f;
  const float cardTop = top + std::max(0.0f, (by - H * 0.03f - top - ch) * 0.5f);
  // The titles share one size (the one the longest needs), as do the ROM lines.
  float titleFs = 1.0f;
  for (int k : cards_)
    titleFs = std::min(titleFs, cw * 0.9f / std::max(1.0f, measure(kCardInfo[k].title, fTitle, true)));
  const std::string romLine = (exact_ ? "\xE2\x9C\x93 " : "! ") + romName_;
  const float romFs = std::min(1.0f, cw * 0.92f / std::max(1.0f, measure(romLine, fSmall, false)));
  cardHits_.assign(N, RectF{});
  buttonHits_.assign(kButtons, RectF{});
  for (int i = 0; i < N; i++) {
    const CardInfo& ci = kCardInfo[cards_[i]];
    Preview& pv = prev_[cards_[i]];
    float an = anim_[i];
    float sc = 1.0f + 0.035f * an;
    float cx = x0 + i * (cw + gap) + cw * 0.5f, cy = cardTop + ch * 0.5f;
    RectF r = rectF(cx - cw * sc * 0.5f, cy - ch * sc * 0.5f, cx + cw * sc * 0.5f, cy + ch * sc * 0.5f);
    const float rw = r.right - r.left;
    cardHits_[i] = r;
    ColorF c = gameAccent(ci.game);
    float rad = H * 0.02f;
    if (an > 0.01f) {
      float glow = H * 0.012f * an;
      menu.fillRound(rectF(r.left - glow, r.top - glow, r.right + glow, r.bottom + glow), rad + glow,
                     withA(c, 0.25f * an));
    }
    menu.fillRound(r, rad, argb(0xFF141824));
    float pw = rw - rad * 1.2f;
    RectF pr = rectF(r.left + rad * 0.6f, r.top + rad * 0.6f, 0, 0);
    pr.right = pr.left + pw;
    pr.bottom = pr.top + pw / aspect;
    menu.fillRound(pr, rad * 0.5f, argb(0xFF0A0D14));
    if (hasCover(cards_[i]) && haveRom_) {
      // The player's cover, filling its frame (what's beyond it trimmed, a
      // tall cover's mostly from the bottom: box art's logo is at the top),
      // or whole (the dark backdrop either side); dimmed when not chosen.
      Art& a = cover_[cards_[i]];
      if (int img = artImage(cv, a)) {
        const float aw = pr.right - pr.left, ah = pr.bottom - pr.top;
        const float s = coverFill_ ? std::max(aw / a.w, ah / a.h) : std::min(aw / a.w, ah / a.h);
        const float dw = a.w * s, dh = a.h * s;
        const float x = pr.left + (aw - dw) * 0.5f, y = pr.top + (ah - dh) * (dh > ah ? 0.25f : 0.5f);
        cv->pushClip(pr);
        cv->drawImage(img, rectF(x, y, x + dw, y + dh), 1.0f, true);
        cv->popClip();
        if (an < 0.99f) fill(pr, colorF(0, 0, 0, 0.35f * (1.0f - an)));
      }
    } else if (pv.snes && pv.ready) {
      // The game's 256 px picture (its frame buffer is wider: the margins either side).
      int fh = pv.snes->ppu.frameHeight();
      if (pv.image && pv.imageH != fh) {
        cv->freeImage(pv.image);
        pv.image = 0;
      }
      if (!pv.image) {
        pv.image = cv->makeImage(256, fh);
        pv.imageH = fh;
        pv.fresh = true;
      }
      if (pv.image) {
        // (only a picture drawn since: the others are as they were)
        if (pv.fresh) cv->updateImage(pv.image, pv.snes->ppu.frame + kMaxExt, kLineW);
        pv.fresh = false;
        // (whole multiples of the picture stay sharp; in between, smooth)
        // (in a square frame beside covers: the picture its own shape, centred)
        RectF ip = pr;
        if (aspect != screenAspect) {
          const float ih = (pr.right - pr.left) / screenAspect, my = (pr.top + pr.bottom) * 0.5f;
          ip.top = my - ih * 0.5f;
          ip.bottom = my + ih * 0.5f;
        }
        float scale = (ip.bottom - ip.top) / fh;
        cv->drawImage(pv.image, ip, pv.shown, scale < 2.0f);
        if (an < 0.99f) fill(pr, colorF(0, 0, 0, 0.35f * (1.0f - an)));
      }
    } else if (pv.snes) {
      text((pr.left + pr.right) * 0.5f, (pr.top + pr.bottom) * 0.5f - lh(fSmall) * 0.5f, "Starting\xE2\x80\xA6", fSmall,
           false, argb(0xFF5A6070), 1);
    } else {
      // What is missing, shrunk to fit the preview area.
      const std::string head = "ROM not found";
#ifdef _WIN32
      std::vector<std::string> lines = {"Place Super Mario All-Stars (USA).sfc", "in the game folder or a \"roms\" folder",
                                        "(CRC32 925637C7, 2048 KB; zipped is fine)."};
#elif defined(__SWITCH__)
      std::vector<std::string> lines = {"Put your Super Mario All-Stars (USA)", "ROM (.sfc or .smc) in the same folder",
                                        "as this .nro on the SD card."};
#elif !defined(__ANDROID__)
      std::vector<std::string> lines = {"Click here to choose your ROM, or put", "Super Mario All-Stars (USA).sfc in a",
                                        "\"roms\" folder beside the game."};
#else
      std::vector<std::string> lines = {"Tap here to choose your", "Super Mario All-Stars (USA) ROM",
                                        "(.sfc or .smc, 2048 KB)."};
#endif
      float bw = 1.0f;
      for (const auto& l : lines) bw = std::max(bw, measure(l, fSmall, false));
      float hs = std::min(1.0f, pw * 0.9f / std::max(1.0f, measure(head, fTitle, true)));
      float bs = std::min(1.0f, pw * 0.9f / bw);
      float th = lh(fTitle, hs) * 1.15f + lh(fSmall, bs) * lines.size();
      const float vs = std::min(1.0f, (pr.bottom - pr.top) * 0.9f / th);
      hs *= vs;
      bs *= vs;
      float y = pr.top + ((pr.bottom - pr.top) - th * vs) * 0.5f;
      text((pr.left + pr.right) * 0.5f, y, head, fTitle, true, argb(0xFFE06A6A), 1, hs);
      y += lh(fTitle, hs) * 1.15f;
      for (const auto& l : lines) {
        text((pr.left + pr.right) * 0.5f, y, l, fSmall, false, argb(0xFFB0B8C8), 1, bs);
        y += lh(fSmall, bs);
      }
    }
    float ty = pr.bottom + rad * 0.7f;
    text(cx, ty, ci.title, fTitle, true, an > 0.5f ? c : argb(0xFFE6E9F0), 1, titleFs);
    ty += lh(fTitle, titleFs) * 0.95f;
    float sfs = std::min(1.0f, rw * 0.94f / std::max(1.0f, measure(ci.subtitle, fSmall, false)));
    text(cx, ty, ci.subtitle, fSmall, false, argb(0xFF9AA4BA), 1, sfs);
    ty += lh(fSmall) * 1.05f;
    std::string meta = std::string(ci.year) + "  \xE2\x80\xA2  " + (ci.players > 1 ? "1-2 Players" : "1 Player");
    text(cx, ty, meta, fSmall, false, ci.players > 1 ? lerp(c, colorF(1, 1, 1), 0.3f) : argb(0xFF7C8598), 1);
    ty += lh(fSmall) * 1.1f;
    if (haveRom_) text(cx, ty, romLine, fSmall, false, exact_ ? argb(0xFF6FCF8A) : argb(0xFFF0B060), 1, romFs);
    menu.outlineRound(r, rad, std::max(1.5f, H / 400.0f) * (0.5f + an),
                      withA(an > 0.01f ? c : argb(0xFF2A3040), 0.6f + 0.4f * an));
  }

  // Buttons, in the menus' style: Storybook's cream pills outlined in brown
  // (yellow with a star when chosen) and Luckiest Guy; Retro's boxes with a
  // gold arrow and pixels; Themed Dark's pills with a star and Luckiest Guy;
  // Classic's plain pills.
  const int style = menu.style;
  const bool story = style == Menu::kStorybook, retro = style == Menu::kRetro, themed = style == Menu::kThemedDark;
  const bool display = story || themed;  // (labels in the display face)
  const ColorF ink = argb(0xFF3A1D0E), gold = argb(0xFFFCBC3C);
  const float inkW = std::max(2.0f, H / 300.0f);
  const float bh = lh(fBody) * 1.6f;
  const auto labelSize = [&](float scale) { return (display ? fBody * 1.12f : retro ? fBody * 0.8f : fBody) * scale; };
  const auto labelW = [&](const char* t, float scale) {
    const std::wstring w = Menu::widen(t);
    return display || retro ? menu.displayMeasure(w, labelSize(scale)) : measure(t, fBody * scale, false);
  };
  float bws[kButtons], total = 0;
  for (int i = 0; i < kButtons; i++) {
    bws[i] = labelW(kButtonNames[i], 1.0f) + bh * (story || themed || retro ? 1.9f : 1.4f);
    total += bws[i];
  }
  float bgap = W * 0.02f;
  // the row shrinks to fit narrow (4:3) windows
  const float bs = std::min(1.0f, W * 0.96f / (total + bgap * (kButtons - 1)));
  for (float& bw : bws) bw *= bs;
  total *= bs;
  bgap *= bs;
  float bx = (W - total - bgap * (kButtons - 1)) * 0.5f;
  for (int i = 0; i < kButtons; i++) {
    float an = anim_[N + i];
    RectF br = rectF(bx, by + bh * (1.0f - bs) * 0.5f, bx + bws[i], 0);
    br.bottom = br.top + bh * bs;
    buttonHits_[i] = br;
    const float bhh = br.bottom - br.top, cy = (br.top + br.bottom) * 0.5f;
    const std::wstring name = Menu::widen(kButtonNames[i]);
    // (the label's box: past the star or arrow when chosen)
    RectF lr = br;
    if (story) {
      menu.fillRound(br, bhh * 0.5f, lerp(argb(0xFFFFF8E4), argb(0xFFFFD400), an));
      menu.outlineRound(br, bhh * 0.5f, inkW, ink);
      if (an > 0.01f) {
        menu.star(br.left + bhh * 0.55f, cy + std::sin(time_ * 5.0f) * bhh * 0.04f, bhh * 0.3f * an,
                  argb(0xFFFFFFFF), ink, inkW * 0.7f);
        lr.left += bhh * 0.45f * an;
      }
      menu.displayText(name, lr, labelSize(bs), ink, ColorF{}, 0.0f, kAlignCenter);
    } else if (retro) {
      cv->fillRect(br, argb(0xFF000000));
      cv->strokeRect(br, inkW, an > 0.5f ? gold : argb(0xFFFCFCFC));
      if (an > 0.5f) {
        const float cs = labelSize(bs) * 0.5f, ax = br.left + bhh * 0.35f;
        const PointF tri[3] = {{ax, cy - cs * 0.7f}, {ax + cs, cy}, {ax, cy + cs * 0.7f}};
        cv->fillPolygon(tri, 3, gold);
        lr.left += bhh * 0.45f;
      }
      menu.displayText(name, lr, labelSize(bs), an > 0.5f ? gold : argb(0xFFFCFCFC), ColorF{}, 0.0f, kAlignCenter);
    } else {
      menu.fillRound(br, bhh * 0.5f, lerp(argb(0xFF1C2230), acc, an * 0.85f));
      if (themed) {
        if (an > 0.01f) {
          menu.star(br.left + bhh * 0.5f, cy, bhh * 0.26f * an, argb(0xFFFFD83D), argb(0xFFB8860B),
                    std::max(1.0f, H / 900.0f));
          lr.left += bhh * 0.4f * an;
        }
        menu.displayText(name, lr, labelSize(bs), an > 0.5f ? argb(0xFF10131A) : argb(0xFFFFFFFF), ColorF{}, 0.0f,
                         kAlignCenter);
      } else {
        text((br.left + br.right) * 0.5f, br.top + (bhh - lh(fBody, bs)) * 0.5f, kButtonNames[i], fBody, false,
             an > 0.5f ? argb(0xFF10131A) : argb(0xFFD8DDE8), 1, bs);
      }
    }
    bx += bws[i] + bgap;
  }
  const std::string keys = !hint_.empty()  ? hint_
                           : confirmRight_ ? "\xE2\x86\x90\xE2\x86\x92 Choose    Enter / (B) Play    F11 Fullscreen    Esc Quit"
                                           : "\xE2\x86\x90\xE2\x86\x92 Choose    Enter / (A) Play    F11 Fullscreen    Esc Quit";
  if (retro)
    menu.pixelText(Menu::widen(keys), rectF(0, H - lh(fSmall) * 1.6f, W, H - lh(fSmall) * 0.6f), fSmall * 0.78f,
                   argb(0xFFBCBCBC), kAlignCenter, kParaNear);
  else
    text(W * 0.5f, H - lh(fSmall) * 1.6f, keys, fSmall, story, argb(0xFFA3B0CC), 1);
}

bool Launcher::overLink(float x, float y) const {
  return x >= linkHit_.left && x < linkHit_.right && y >= linkHit_.top && y < linkHit_.bottom;
}

Launcher::Action Launcher::tap(float x, float y) {
  auto in = [&](const RectF& r) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
  if (!updateTag_.empty() && in(updateHit_)) return kUpdate;
  if (overLink(x, y)) return kPatreon;
  const int N = (int)cards_.size();
  for (int i = 0; i < (int)cardHits_.size() && i < N; i++)
    if (in(cardHits_[i])) {
      if (sel_ == i) return kPlay;
      sel_ = lastCard_ = i;
      return kNone;
    }
  static const Action kActs[kButtons] = {kOptions, kAchievements, kMusic, kCredits, kQuit};
  for (int i = 0; i < (int)buttonHits_.size(); i++)
    if (in(buttonHits_[i])) {
      sel_ = N + i;
      return kActs[i];
    }
  return kNone;
}
