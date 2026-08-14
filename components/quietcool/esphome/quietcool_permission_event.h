#pragma once

#include "esphome/components/event/event.h"
#include "esphome/core/component.h"

namespace esphome::quietcool {

// The Home Assistant-visible signal for a permission-to-start refusal (see
// QuietCoolComponent::set_start_refused_event()'s declaration for why an
// `event` entity rather than a text sensor). No state of its own beyond what
// event::Event already provides — trigger() is called directly by the
// controller from request_state()'s gate, so this class exists only to give
// that entity a Component identity (dump_config, registration) matching
// every other QuietCool entity's shape.
class QuietCoolStartRefusedEvent final : public Component, public event::Event {
 public:
  void dump_config() override;
};

}  // namespace esphome::quietcool
