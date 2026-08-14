// Adapter-layer tests for the permission-to-start failsafe.
//
// The whole-house fan this firmware drives can damage itself or the house if
// it starts with too few windows open for makeup air. permission_to_start_
// exists so the controller refuses to originate a start command — at the
// adapter layer, before the platform-free core ever sees the request — until
// something has established it is safe: either Home Assistant has explicitly
// granted permission (the switch entity's write_state path), or the
// mandatory, non-energizing on-boot refresh found the fan already confirmed
// running (so this controller did not start it, and there is nothing new to
// protect against). Off and Refresh are never gated: a failsafe that could
// itself block stopping the fan would be worse than none at all.

#include "quietcool/esphome/quietcool_component.h"

#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"

#include "support/test.h"
#include "support/test_doubles.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome::quietcool {
namespace {

using ::quietcool::Duration;
using ::quietcool::FanState;
using ::quietcool::FrameBytes;
using ::quietcool::Speed;

constexpr std::uint32_t kSenderSeed = 0xCB004739U;
constexpr std::uint32_t kPreferenceKey = 0x51434332U;
constexpr std::uint32_t kJitterSeed = 0x51434332U;

// Owns the stub NVS for one test and restores the global on the way out —
// copied from radio_counters_test.cpp / component_deferral_test.cpp.
class ScopedPreferences final {
 public:
  ScopedPreferences() {
    previous_ = global_preferences;
    global_preferences = &preferences_;
  }
  ~ScopedPreferences() { global_preferences = previous_; }

 private:
  ESPPreferences preferences_;
  ESPPreferences* previous_{nullptr};
};

// The exact 6-byte OEM state report for kSenderSeed (0xCB004739) carrying
// 0xDF: Speed::Low + Duration::Continuous per fan_state.cpp's bit layout
// (raw & 0x0F == 0xF == Continuous; (raw >> 4) & 0x03 == 1 == Low) — an ON
// state, matching the 0xDF convention core_timeline_test.cpp and others in
// this suite already use for "the fan is running".
FrameBytes on_report_frame() {
  return {{0xCB, 0x00, 0x47, 0x39, 0xDF, 0xDF}};
}

QC_TEST("adapter",
       "permission_to_start defaults false and refuses a start command "
       "before it reaches the core") {
  // Deliberately NOT provisioned (no setup()): if the gate under test did
  // NOT intercept the request before core_.request_state(), the real core
  // would refuse it anyway for being Unprovisioned — and that refusal
  // publishes a CoreEvent (RequestRefused -> "refused" on the command status
  // sensor, see esp_event_sink.cpp). Asserting the sensor stays untouched by
  // the ON command is therefore the discriminating check: it proves the
  // request never reached the core at all, not merely that the core also
  // would have said no.
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey,
                               kJitterSeed);
  text_sensor::TextSensor command_status;
  component.set_command_status_sensor(&command_status);

  QC_CHECK(!component.permission_to_start());
  component.request_state(FanState::command(Speed::High, Duration::Hours1));
  QC_CHECK(command_status.published().empty());
  QC_CHECK(radio.packets().empty());

  // Off must still pass straight through to the core, ungated, even though
  // the same unprovisioned core will itself refuse it — that refusal IS the
  // "reached the core" signal this test needs.
  component.request_state(FanState::command(Speed::Low, Duration::Off));
  QC_CHECK(!command_status.published().empty());
  QC_CHECK_EQ(command_status.published().back(), std::string("refused"));
}

QC_TEST("adapter",
       "granting permission lets a start command reach the core and "
       "transmit") {
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey,
                               kJitterSeed);
  host_test::set_millis(0);
  // Binds provisioned_sender_ via the compiled seed, matching
  // radio_counters_test.cpp's "rx_valid_count_increments..." setup.
  component.setup();

  // Drain the boot query burst (3 physical frames) so the radio's packet
  // count below counts only what THIS test's command transmits.
  for (std::size_t pass = 0; pass < 30 && radio.packets().size() < 3;
       ++pass) {
    host_test::advance_millis(50);
    component.call_loop();
  }
  QC_CHECK_EQ(radio.packets().size(), std::size_t(3));

  QC_CHECK(!component.permission_to_start());
  component.request_state(FanState::command(Speed::High, Duration::Hours1));
  for (std::size_t pass = 0; pass < 30 && radio.packets().size() < 4;
       ++pass) {
    host_test::advance_millis(50);
    component.call_loop();
  }
  // Still refused: permission was never granted, so no 4th frame goes out
  // beyond the boot query's 3.
  QC_CHECK_EQ(radio.packets().size(), std::size_t(3));

  component.set_permission_to_start(true);
  QC_CHECK(component.permission_to_start());
  component.request_state(FanState::command(Speed::High, Duration::Hours1));
  for (std::size_t pass = 0; pass < 30 && radio.packets().size() < 4;
       ++pass) {
    host_test::advance_millis(50);
    component.call_loop();
  }
  // Now the command actually reaches the radio.
  QC_CHECK(radio.packets().size() >= 4);
}

QC_TEST("adapter",
       "revoking permission blocks a later start command again") {
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey,
                               kJitterSeed);
  text_sensor::TextSensor command_status;
  component.set_command_status_sensor(&command_status);
  host_test::set_millis(0);
  component.setup();
  for (std::size_t pass = 0; pass < 30 && radio.packets().size() < 3;
       ++pass) {
    host_test::advance_millis(50);
    component.call_loop();
  }

  component.set_permission_to_start(true);
  component.set_permission_to_start(false);
  QC_CHECK(!component.permission_to_start());

  const auto before = command_status.published().size();
  component.request_state(FanState::command(Speed::High, Duration::Hours1));
  // The gate refused it silently again — no new command-status publication
  // and no additional radio traffic.
  QC_CHECK_EQ(command_status.published().size(), before);
  QC_CHECK_EQ(radio.packets().size(), std::size_t(3));
}

QC_TEST("adapter",
       "set_permission_switch replays the current value to a newly "
       "registered switch") {
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey,
                               kJitterSeed);
  switch_::Switch late_switch;

  // Registered before any grant: should reflect the false default.
  component.set_permission_switch(&late_switch);
  QC_CHECK(!late_switch.published().empty());
  QC_CHECK(!late_switch.published().back());

  component.set_permission_to_start(true);
  QC_CHECK(!late_switch.published().empty());
  QC_CHECK(late_switch.published().back());
}

QC_TEST("adapter",
       "an on-boot refresh confirming the fan already running auto-grants "
       "permission to start") {
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey,
                               kJitterSeed);
  switch_::Switch permission_switch;
  component.set_permission_switch(&permission_switch);
  host_test::set_millis(0);
  component.setup();

  // Drive the boot query burst to completion (BootQueryTransmitting ->
  // BootResponseListening happens on the burst's tx-complete callback, which
  // apply_burst_event() delivers as soon as BurstComplete fires — see
  // component_deferral_test.cpp's identical drive-to-3-frames pattern).
  for (std::size_t pass = 0; pass < 30 && radio.packets().size() < 3;
       ++pass) {
    host_test::advance_millis(50);
    component.call_loop();
  }
  QC_CHECK_EQ(radio.packets().size(), std::size_t(3));
  QC_CHECK(!component.permission_to_start());

  // Move well into the boot query's acceptance window
  // (kDirectQueryAcceptStartMs=300 .. kDirectQueryAcceptEndMs=1100 after the
  // burst's start) before the first response candidate, then send two
  // independent, agreeing "fan is ON" reports kMinIndependentCandidateGapMs
  // (60 ms) apart — the same two-frame consensus pattern
  // core_timeline_test.cpp's consensus() helper uses.
  host_test::advance_millis(500);
  const auto report = on_report_frame();
  component.on_radio_packet(::quietcool::ByteView(report.bytes));
  host_test::advance_millis(65);
  component.on_radio_packet(::quietcool::ByteView(report.bytes));

  QC_CHECK(component.permission_to_start());
  QC_CHECK(!permission_switch.published().empty());
  QC_CHECK(permission_switch.published().back());

  // Confirmation quarantines the coordinator (ResponseTailQuarantine) for
  // the remainder of the response-tail window before it will accept a new
  // request — the same kResponseTailEndMs (2500 ms from the query's anchor)
  // core_timeline_test.cpp's REG-D case runs out. Clear it the same way:
  // advance well past the tail before issuing the start command, so this
  // test is asserting the permission gate specifically, not racing the
  // quarantine window.
  for (std::size_t pass = 0; pass < 80; ++pass) {
    host_test::advance_millis(50);
    component.call_loop();
  }

  // And the auto-grant actually unblocks a start command, with no manual
  // grant needed.
  const auto before = radio.packets().size();
  component.request_state(FanState::command(Speed::High, Duration::Hours1));
  for (std::size_t pass = 0; pass < 30 && radio.packets().size() <= before;
       ++pass) {
    host_test::advance_millis(50);
    component.call_loop();
  }
  QC_CHECK(radio.packets().size() > before);
}

QC_TEST("adapter",
       "a later confirmation that the fan is running does NOT retroactively "
       "grant permission") {
  // Only the on-boot refresh's own BootQueryConsensus may auto-grant (item 3
  // of the design is specifically about what the FIRST refresh after boot
  // finds, not "the fan is confirmed on at some point"). A manual Refresh
  // later confirming the fan is running must not silently open the failsafe
  // — Home Assistant must still grant it explicitly in that case.
  ::quietcool::AuthoritySnapshot manual_confirmed{};
  manual_confirmed.state = ::quietcool::ConfirmedStateAuthority{
      FanState::command(Speed::Low, Duration::Continuous),
      ::quietcool::EvidenceSource::ManualQueryConsensus,
      ::quietcool::EvidenceConfidence::ExactBackedConsensus,
      0, 2, std::nullopt, std::nullopt, 0};
  manual_confirmed.revision = 0;

  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey,
                               kJitterSeed);
  component.publish_authority_for_test(manual_confirmed);

  QC_CHECK(!component.permission_to_start());
}

}  // namespace
}  // namespace esphome::quietcool
