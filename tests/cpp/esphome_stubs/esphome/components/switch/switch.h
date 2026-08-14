#pragma once

// Host stub. Records published values so tests can assert what Home
// Assistant would have been told. This stub covers only the surface
// quietcool_component.{h,cpp} touches (a bare pointer and publish_state()) —
// the real switch_::Switch base (write_state, control/turn_on/turn_off,
// restore_mode, inversion) is exercised only via `esphome compile` in CI,
// exactly like button_::Button and fan::Fan (see esphome_stubs/README.md and
// the ADAPTER_SOURCES exclusion in tests/cpp/Makefile). Do not grow this
// stub to also model write_state()/control(): the moment it needs to, that
// is the same signal the README calls out — the real class belongs on the
// exclusion list instead of behind a stub good enough to be wrong.

#include <vector>

namespace esphome::switch_ {

class Switch {
 public:
  virtual ~Switch() = default;

  virtual void publish_state(bool state) {
    state_ = state;
    has_state_ = true;
    published_.push_back(state);
  }

  bool state() const { return state_; }
  bool has_state() const { return has_state_; }
  const std::vector<bool>& published() const { return published_; }
  void clear_published() { published_.clear(); }

 private:
  bool state_{false};
  bool has_state_{false};
  std::vector<bool> published_;
};

}  // namespace esphome::switch_
