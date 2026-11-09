#include "core/rewind.h"
#include <algorithm>
#include <cstring>

namespace {
void put32(std::vector<uint8_t>& o, uint32_t v) {
  for (int i = 0; i < 4; i++) o.push_back((uint8_t)(v >> (8 * i)));
}
bool get32(const uint8_t*& p, const uint8_t* end, uint32_t* v) {
  if (end - p < 4) return false;
  *v = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
  p += 4;
  return true;
}
}  // namespace

// Format: older size, newer size, then (zero run, literal count, literals)
// pairs, each count a 32-bit value, covering max(sizes) bytes of
// older XOR newer (the shorter one padded with zeros).
//
// Two consecutive states differ in a few hundred bytes out of ~270 KB, so the
// scan skips the equal stretches eight bytes at a time (this runs 30 times a
// second, on the emulation's own thread). The bytes written are exactly those
// of the plain byte-by-byte scan, kept below as encodeSlow for sizes that differ.
namespace {
inline uint64_t load64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

// The first index at or after `pos` where a and b differ, or n.
inline size_t nextDiff(const uint8_t* a, const uint8_t* b, size_t pos, size_t n) {
  while (pos + 8 <= n) {
    const uint64_t d = load64(a + pos) ^ load64(b + pos);
    if (d) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
      return pos + (size_t)(__builtin_clzll(d) >> 3);
#else
      return pos + (size_t)(__builtin_ctzll(d) >> 3);
#endif
    }
    pos += 8;
  }
  while (pos < n && a[pos] == b[pos]) pos++;
  return pos;
}
}  // namespace

std::vector<uint8_t> Rewind::encodeSlow(const std::vector<uint8_t>& older, const std::vector<uint8_t>& newer) {
  std::vector<uint8_t> o;
  size_t n = std::max(older.size(), newer.size());
  o.reserve(64);
  put32(o, (uint32_t)older.size());
  put32(o, (uint32_t)newer.size());
  auto x = [&](size_t i) -> uint8_t {
    return (i < older.size() ? older[i] : 0) ^ (i < newer.size() ? newer[i] : 0);
  };
  size_t i = 0;
  while (i < n) {
    size_t z = i;
    while (z < n && x(z) == 0) z++;
    size_t l = z;
    // (a literal run ends at 8 zero bytes in a row)
    size_t zeros = 0;
    while (l < n && zeros < 8) {
      zeros = x(l) == 0 ? zeros + 1 : 0;
      l++;
    }
    if (zeros == 8) l -= 8;
    put32(o, (uint32_t)(z - i));
    put32(o, (uint32_t)(l - z));
    for (size_t k = z; k < l; k++) o.push_back(x(k));
    i = l;
  }
  return o;
}

std::vector<uint8_t> Rewind::encode(const std::vector<uint8_t>& older, const std::vector<uint8_t>& newer) {
  if (older.size() != newer.size()) return encodeSlow(older, newer);
  const size_t n = older.size();
  const uint8_t* a = older.data();
  const uint8_t* b = newer.data();
  std::vector<uint8_t> o;
  o.reserve(4096);
  put32(o, (uint32_t)n);
  put32(o, (uint32_t)n);
  size_t i = 0;
  while (i < n) {
    const size_t z = nextDiff(a, b, i, n);  // the zero run ends at the first byte that differs
    size_t l = z;
    if (z < n) {
      // the literal run: up to the last differing byte that is followed by 8 equal ones (or by
      // fewer than 8 and then the end, which are kept)
      size_t p = z;
      for (;;) {
        const size_t q = p + 1;
        const size_t nz = nextDiff(a, b, q, n);
        if (nz - q >= 8) {
          l = q;
          break;
        }
        if (nz == n) {
          l = n;
          break;
        }
        p = nz;
      }
    }
    put32(o, (uint32_t)(z - i));
    put32(o, (uint32_t)(l - z));
    const size_t at = o.size();
    o.resize(at + (l - z));
    uint8_t* w = o.data() + at;
    for (size_t k = z; k < l; k++) w[k - z] = a[k] ^ b[k];
    i = l;
  }
  return o;
}

bool Rewind::decode(const std::vector<uint8_t>& delta, const std::vector<uint8_t>& newer,
                    std::vector<uint8_t>* older) {
  const uint8_t* p = delta.data();
  const uint8_t* end = p + delta.size();
  uint32_t oldN, newN;
  if (!get32(p, end, &oldN) || !get32(p, end, &newN) || newN != newer.size()) return false;
  size_t n = std::max<size_t>(oldN, newN);
  std::vector<uint8_t> out(n, 0);
  std::memcpy(out.data(), newer.data(), newer.size());
  size_t i = 0;
  while (p < end) {
    uint32_t z, l;
    if (!get32(p, end, &z) || !get32(p, end, &l)) return false;
    i += z;
    if (i + l > n || (size_t)(end - p) < l) return false;
    for (uint32_t k = 0; k < l; k++) out[i + k] ^= p[k];
    p += l;
    i += l;
  }
  out.resize(oldN);
  *older = std::move(out);
  return true;
}

void Rewind::clear() {
  cur_.clear();
  deltas_.clear();
  bytes_ = 0;
}

void Rewind::push(const std::vector<uint8_t>& state) {
  if (!cur_.empty()) {
    deltas_.push_back(encode(cur_, state));
    bytes_ += deltas_.back().size();
    while (!deltas_.empty() && (bytes_ > maxBytes_ || deltas_.size() + 1 > maxSnapshots_)) {
      bytes_ -= deltas_.front().size();
      deltas_.pop_front();
    }
  }
  cur_ = state;
}

bool Rewind::stepBack(std::vector<uint8_t>* out) {
  if (deltas_.empty()) return false;
  std::vector<uint8_t> older;
  if (!decode(deltas_.back(), cur_, &older)) {
    clear();
    return false;
  }
  bytes_ -= deltas_.back().size();
  deltas_.pop_back();
  cur_ = std::move(older);
  *out = cur_;
  return true;
}
