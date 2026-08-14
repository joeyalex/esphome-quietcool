#include "quietcool_permission_switch.h"

#include "esphome/core/log.h"

namespace esphome::quietcool {
namespace {

constexpr char TAG[] = "quietcool.permission_switch";

}  // namespace

void QuietCoolPermissionSwitch::dump_config() {
  LOG_SWITCH("", "QuietCool Permission To Start", this);
}

void QuietCoolPermissionSwitch::write_state(bool state) {
  if (controller_ == nullptr) {
    ESP_LOGE(TAG, "Switch write refused: controller is not configured");
    return;
  }
  // The controller is the single source of truth for permission_to_start_
  // (it may also flip this true on its own via the on-boot auto-grant path).
  // set_permission_to_start() publishes this entity's state itself once the
  // controller has recorded the change, rather than this method calling
  // publish_state() directly — so an auto-grant racing a manual write can
  // never leave the switch showing something the controller does not
  // actually believe.
  controller_->set_permission_to_start(state);
}

}  // namespace esphome::quietcool
