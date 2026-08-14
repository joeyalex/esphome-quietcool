#include "quietcool_permission_event.h"

namespace esphome::quietcool {
namespace {

constexpr char TAG[] = "quietcool.permission_event";

}  // namespace

void QuietCoolStartRefusedEvent::dump_config() {
  LOG_EVENT("", "QuietCool Start Refused", this);
}

}  // namespace esphome::quietcool
