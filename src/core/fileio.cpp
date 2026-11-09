#include "fileio.h"
#include <cstdint>
#include <cstdio>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef _WIN32
bool writeFileSafely(const std::wstring& path, const void* data, size_t n) {
  std::wstring tmp = path + L".tmp";
  FILE* f = _wfopen(tmp.c_str(), L"wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, n, f) == n;
  ok = fflush(f) == 0 && ok;
  ok = fclose(f) == 0 && ok;
  if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
  if (!ok) DeleteFileW(tmp.c_str());
  return ok;
}

bool writeFileSafely(const std::string& path, const void* data, size_t n) {
  UINT cp = CP_UTF8;
  int len = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0);
  if (len <= 0) {
    cp = CP_ACP;
    len = MultiByteToWideChar(cp, 0, path.c_str(), -1, nullptr, 0);
    if (len <= 0) return false;
  }
  std::wstring w(len, 0);
  MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, path.c_str(), -1, w.data(), len);
  w.resize(len - 1);  // (drop the terminator)
  return writeFileSafely(w, data, n);
}
#else
bool writeFileSafely(const std::string& path, const void* data, size_t n) {
  std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(data, 1, n, f) == n;
  ok = fflush(f) == 0 && ok;
  ok = fclose(f) == 0 && ok;
  if (ok) {
    ok = std::rename(tmp.c_str(), path.c_str()) == 0;
#ifdef __SWITCH__
    // (the SD card's file system will not rename over a file that is there: the old one goes first)
    if (!ok) {
      std::remove(path.c_str());
      ok = std::rename(tmp.c_str(), path.c_str()) == 0;
    }
#endif
  }
  if (!ok) std::remove(tmp.c_str());
  return ok;
}

// (a path of code points, to UTF-8)
bool writeFileSafely(const std::wstring& path, const void* data, size_t n) {
  std::string s;
  for (wchar_t wc : path) {
    uint32_t c = (uint32_t)wc;
    if (c < 0x80) {
      s += (char)c;
    } else if (c < 0x800) {
      s += (char)(0xc0 | c >> 6);
      s += (char)(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
      s += (char)(0xe0 | c >> 12);
      s += (char)(0x80 | ((c >> 6) & 0x3f));
      s += (char)(0x80 | (c & 0x3f));
    } else {
      s += (char)(0xf0 | c >> 18);
      s += (char)(0x80 | ((c >> 12) & 0x3f));
      s += (char)(0x80 | ((c >> 6) & 0x3f));
      s += (char)(0x80 | (c & 0x3f));
    }
  }
  return writeFileSafely(s, data, n);
}
#endif
