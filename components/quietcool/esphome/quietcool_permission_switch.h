#pragma once

#include "quietcool_component.h"

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"

namespace esphome::quietcool {

// The Home Assistant / ESP32-API face of the permission-to-start failsafe.
// Deliberately a thin marshalling shell (issue #15's rule, same as
// QuietCoolButton and QuietCoolFan): write_state() forwards the operator's
// decision straight to the controller, and every actual gating decision
// lives in QuietCoolComponent::request_state() and
// maybe_auto_grant_permission_to_start(), which are host-tested. The one
// piece of behavior that belongs here rather than in the controller is
// restore_mode — set to ALWAYS_OFF in switch.py so this entity, like the
// underlying flag, can never come back true on its own after a reboot.
class QuietCoolPermissionSwitch final : public Component,
                                        public switch_::Switch {
 public:
  void set_controller(QuietCoolComponent* controller) {
    controller_ = controller;
  }
  void dump_config() override;

 protected:
  void write_state(bool state) override;

 private:
  QuietCoolComponent* controller_{nullptr};
};

}  // namespace esphome::quietcool
