#pragma once

// Host stub. Records triggered event types so tests can assert what Home
// Assistant would have been told. Covers only the surface
// quietcool_component.{h,cpp} touches (a bare pointer and trigger()) — the
// real event::Event base (event_types, last_event_type tracking) is
// exercised only via `esphome compile` in CI, exactly like switch_::Switch
// (see esphome_stubs/README.md and the ADAPTER_SOURCES exclusion in
// tests/cpp/Makefile).

#include <string>
#include <vector>

namespace esphome::event {

class Event {
 public:
  virtual ~Event() = default;

  virtual void trigger(const std::string& event_type) {
    triggered_.push_back(event_type);
  }

  const std::vector<std::string>& triggered() const { return triggered_; }
  void clear_triggered() { triggered_.clear(); }

 private:
  std::vector<std::string> triggered_;
};

}  // namespace esphome::event
