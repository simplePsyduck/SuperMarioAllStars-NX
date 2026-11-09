// Rewind buffer: the recent past as a chain of save states. Only the newest
// is kept whole; each older one is stored as its difference from the next
// (XOR, zero runs packed), a few KB instead of ~270 KB.
#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

class Rewind {
public:
  // At most `maxBytes` of differences and `maxSnapshots` states are kept;
  // the oldest go first.
  explicit Rewind(size_t maxBytes = 96u << 20, size_t maxSnapshots = 1800)
      : maxBytes_(maxBytes), maxSnapshots_(maxSnapshots) {}

  void clear();
  // Records `state` as the newest snapshot.
  void push(const std::vector<uint8_t>& state);
  // Steps back one snapshot: drops the newest and puts the one before it in
  // `out`. False when there is nothing older.
  bool stepBack(std::vector<uint8_t>* out);
  // The newest snapshot (empty if none).
  const std::vector<uint8_t>& newest() const { return cur_; }
  size_t snapshots() const { return cur_.empty() ? 0 : deltas_.size() + 1; }
  size_t bytes() const { return bytes_; }

  // (Exposed for tests.)
  static std::vector<uint8_t> encode(const std::vector<uint8_t>& older, const std::vector<uint8_t>& newer);
  static std::vector<uint8_t> encodeSlow(const std::vector<uint8_t>& older, const std::vector<uint8_t>& newer);
  static bool decode(const std::vector<uint8_t>& delta, const std::vector<uint8_t>& newer,
                     std::vector<uint8_t>* older);

private:
  size_t maxBytes_, maxSnapshots_;
  std::vector<uint8_t> cur_;                 // newest snapshot, whole
  std::deque<std::vector<uint8_t>> deltas_;  // deltas_[i]: snapshot i from snapshot i + 1
  size_t bytes_ = 0;
};
