#pragma once

#include "quietcool/core/confirmation_core.h"
#include "quietcool/ports/core_callback_queue.h"
#include "esp_event_sink.h"
#include "esp_monotonic_clock.h"
#include "preferences_adapter.h"
#include "quietcool/radio/burst_transmitter.h"

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/event/event.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace esphome::quietcool {

class AuthorityPublisher {
 public:
  virtual ~AuthorityPublisher() = default;
  virtual void publish_authority(
      const ::quietcool::AuthoritySnapshot& authority) = 0;
};

class QuietCoolComponent final : public Component {
 public:
  QuietCoolComponent(::quietcool::Radio* radio, std::uint32_t sender_seed,
                     std::uint32_t preference_key, std::uint32_t jitter_seed);

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  // Four covers the fan, the timer select and headroom — same fixed-capacity,
  // no-heap discipline as CoreEffects::kCapacity. An overflow registration is
  // DROPPED rather than displacing a live publisher: silently unsubscribing
  // the fan entity would strand Home Assistant on stale state, which is worse
  // than refusing the newcomer.
  static constexpr std::size_t kMaxAuthorityPublishers = 4;

  void add_authority_publisher(AuthorityPublisher* publisher) {
    if (publisher == nullptr) return;
    if (authority_publisher_count_ >= kMaxAuthorityPublishers) {
      // Loudly, not silently (round 1, opus): the dropped entity keeps its
      // controller_ pointer and can still TRANSMIT — it just never receives a
      // snapshot, so e.g. a dropped timer select would aim every command with
      // no confirmed state and nothing anywhere would say why.
      ESP_LOGE("quietcool", "authority publisher dropped: capacity %u exceeded",
               static_cast<unsigned>(kMaxAuthorityPublishers));
      // Terminal degradation, not mark_failed (rounds 3-4). Round 3 (codex):
      // a dropped entity still holds its controller pointer and can TRANSMIT,
      // so the failure must be loud and latched, not a fan that mysteriously
      // always starts LOW. Round 4 (opus): mark_failed alone left degraded_
      // false — surviving entities kept accepting commands that could never
      // transmit — while ALSO stopping loop(), the worst of both. degrade()
      // is the project's one terminal path (issue #9): Controller Fault
      // raises, every *_Known flag drops, all entry points latch shut, and
      // the condition reads as what it is — a configuration error — in Home
      // Assistant. The branch is unreachable in the shipped configs (two
      // publishers of four).
      degrade("authority publisher capacity exceeded (config error)");
      return;
    }
    authority_publishers_[authority_publisher_count_++] = publisher;
  }
  // Every event-sink setter replays a latched degradation (rounds 5-6, all
  // three finding engines): degrade() can fire during entity WIRING — the
  // publisher-overflow path runs from set_controller, and generated setup
  // orders fan: before binary_sensor: — so a sensor registered after the
  // latch would otherwise never learn the controller is dead, and the
  // overflow's "Controller Fault raises" promise silently depended on
  // registration order.
  void set_state_known_sensor(binary_sensor::BinarySensor* sensor) {
    events_.set_state_known_sensor(sensor);
    if (degraded_) events_.publish_controller_failed();
  }
  void set_timer_program_known_sensor(binary_sensor::BinarySensor* sensor) {
    events_.set_timer_program_known_sensor(sensor);
    if (degraded_) events_.publish_controller_failed();
  }
  void set_timer_remaining_known_sensor(binary_sensor::BinarySensor* sensor) {
    events_.set_timer_remaining_known_sensor(sensor);
    if (degraded_) events_.publish_controller_failed();
  }
  void set_confirmed_off_sensor(binary_sensor::BinarySensor* sensor) {
    events_.set_confirmed_off_sensor(sensor);
    if (degraded_) events_.publish_controller_failed();
  }
  void set_controller_fault_sensor(binary_sensor::BinarySensor* sensor) {
    events_.set_controller_fault_sensor(sensor);
    if (degraded_) events_.publish_controller_failed();
  }
  void set_timer_remaining_sensor(sensor::Sensor* sensor) {
    events_.set_timer_remaining_sensor(sensor);
    if (degraded_) events_.publish_controller_failed();
  }
  void set_command_status_sensor(text_sensor::TextSensor* sensor) {
    events_.set_command_status_sensor(sensor);
    if (degraded_) events_.publish_controller_failed();
  }
  void set_evidence_source_sensor(text_sensor::TextSensor* sensor) {
    events_.set_evidence_source_sensor(sensor);
    if (degraded_) events_.publish_controller_failed();
  }
  // Restored diagnostics (Task 4, issue #28's dropped entities). These five
  // live directly on the component rather than on EspHomeEventSink: their
  // sources (the echo-guard TX byte, the last accepted RX frame,
  // provisioned_sender_) are adapter-local observations events_ never sees.
  void set_last_tx_command_sensor(text_sensor::TextSensor* sensor) {
    last_tx_command_sensor_ = sensor;
  }
  void set_last_rx_frame_sensor(text_sensor::TextSensor* sensor) {
    last_rx_frame_sensor_ = sensor;
  }
  void set_last_confirmed_state_sensor(text_sensor::TextSensor* sensor) {
    last_confirmed_state_sensor_ = sensor;
  }
  void set_speed_capability_sensor(text_sensor::TextSensor* sensor) {
    speed_capability_sensor_ = sensor;
  }
  void set_remote_sender_id_sensor(text_sensor::TextSensor* sensor) {
    remote_sender_id_sensor_ = sensor;
  }
  // The three counters restored in Task 3 (tx_count() etc.); this task only
  // adds their entities and publication.
  void set_tx_count_sensor(sensor::Sensor* sensor) { tx_count_sensor_ = sensor; }
  void set_rx_valid_count_sensor(sensor::Sensor* sensor) {
    rx_valid_count_sensor_ = sensor;
  }
  void set_rx_rejected_count_sensor(sensor::Sensor* sensor) {
    rx_rejected_count_sensor_ = sensor;
  }
  // Permission-to-start failsafe. The flag itself always starts false
  // (field initializer below) and this setter re-publishes whatever the
  // current value is to a newly-registered switch — the same "replay on
  // register" discipline the other setters use above, so a switch wired up
  // after an auto-grant (or after some other entity's on_boot lambda queries
  // it) still shows the true state instead of the switch's own compiled
  // default. This is not a degraded_ replay (permission has nothing to do
  // with terminal degradation); it exists so registration order can never
  // strand the entity on a stale value.
  void set_permission_switch(switch_::Switch* permission_switch) {
    permission_switch_ = permission_switch;
    if (permission_switch_ != nullptr)
      permission_switch_->publish_state(permission_to_start_);
  }
  // Fires a discrete HA "event" entity every time request_state() refuses a
  // start command for lack of permission (see request_state()'s definition).
  // Deliberately NOT routed through the existing Command Confirmation Status
  // text sensor: that sensor only republishes to Home Assistant on a VALUE
  // CHANGE, so a second denial in a row (same "refused" text) would be
  // invisible in the Logbook. event::Event::trigger() always publishes a
  // fresh, timestamped state regardless of repetition, which is what makes a
  // repeated "someone tried to start it without permission" visible.
  void set_start_refused_event(event::Event* start_refused_event) {
    start_refused_event_ = start_refused_event;
  }
  // True once it is safe to energize the fan: either Home Assistant has
  // explicitly granted permission (the switch's write_state path), or the
  // mandatory on-boot refresh found the fan already confirmed running (see
  // the BootQueryConsensus branch in deliver_authority()). Read by
  // request_state() to refuse every start command until one of those has
  // happened; never read to gate Off or Refresh, which must always work.
  bool permission_to_start() const { return permission_to_start_; }
  // The switch entity's write_state() calls this directly for both
  // directions: HA granting permission (windows are open) and HA revoking it
  // (windows are closing) are both legitimate, deliberate operator actions.
  // Revoking only blocks FUTURE start commands — it does not stop a fan that
  // is already running, matching the plain reading of "permission to start".
  void set_permission_to_start(bool granted);
  void on_radio_packet(::quietcool::ByteView packet);
  void request_state(::quietcool::FanState requested);
  void request_manual_refresh();
  void request_learn(::quietcool::LearnMode mode);
  void request_forget();
  ::quietcool::CoreSnapshot snapshot() const;
  // For entities that must refuse work after terminal degradation (round 6):
  // the select is a separate Component, so nothing else tells it the
  // controller died.
  bool degraded() const { return degraded_; }
  ::quietcool::MonotonicMs now_ms() const { return clock_.now_ms(); }

  // Monotonic radio diagnostics, restored after the YAML->C++ migration
  // dropped them: never reset outside a reboot. `tx_count_` is the instrument
  // that once proved this bridge was not jamming the OEM remote — it stayed
  // flat through the remote's retry storm, which showed the storm was not
  // this bridge's own traffic. Losing it removed the evidence.
  std::uint32_t tx_count() const { return tx_count_; }
  std::uint32_t rx_valid_count() const { return rx_valid_count_; }
  std::uint32_t rx_rejected_count() const { return rx_rejected_count_; }

#ifdef QUIETCOOL_HOST_TEST
  // Test seams (issue #9). Overflow of the exact-max effect and callback queues
  // is an invariant breach the real core cannot reach through the public
  // surface — a drain empties the effect queue before the next batch, and
  // callbacks drain one per loop — which is precisely why overflow is terminal.
  // The degradation contract is therefore exercised by forcing each queue to
  // reject the next admission at its real production site.
  void force_effect_queue_overflow_for_test() {
    ::quietcool::CoreEffects filler;
    filler.add(::quietcool::RequestRadioReset{0});
    while (effect_drain_.enqueue(filler, 0)) {
    }
    // enqueue_effects now rejects the batch and degrades; apply_effects returns
    // before draining.
    apply_effects(filler, 0);
  }
  void force_callback_queue_overflow_for_test() {
    while (core_callbacks_.enqueue(::quietcool::CoreCallbackKind::RadioRecovered,
                                   std::nullopt)) {
    }
    // apply_effect(RequestRadioReset) recovers the radio, then fails to enqueue
    // the RadioRecovered callback and degrades — the real line-152 path.
    apply_effect(::quietcool::RequestRadioReset{0});
  }
  // Fills the callback queue without applying anything (issue #22): a
  // subsequent RequestRadioReset in a drained batch then cannot enqueue its
  // RadioRecovered callback and degrades mid-drain.
  void fill_callback_queue_for_test() {
    while (core_callbacks_.enqueue(::quietcool::CoreCallbackKind::RadioRecovered,
                                   std::nullopt)) {
    }
  }
  // Drives a hand-built effect batch through the real apply_effects()/drain()
  // path (issue #22), so a degrade triggered by one effect is observed against
  // the effects that follow it in the same batch. Authority fan-out is
  // exercised through this same seam with a PublishAuthorityEffect batch,
  // rather than a dedicated publish hook.
  // Task 2 deliberately declined this hook while the effect branch was the
  // production path a hand-built PublishAuthorityEffect could exercise. The
  // round-2 delivery fix moved production to the post-drain snapshot, whose
  // source is the real core — un-injectable from a host test — so this hook
  // is now the honest seam onto the SAME publish_authority_snapshot the
  // post-drain lambda calls.
  void publish_authority_for_test(const ::quietcool::AuthoritySnapshot& a) {
    deliver_authority(a, clock_.now_ms());
  }
  void drive_effects_for_test(const ::quietcool::CoreEffects& effects) {
    apply_effects(effects, 0);
  }
  // Degrades directly, without manipulating any queue (issue #23).
  void degrade_for_test() { degrade("test-forced degradation"); }
#endif

 private:
  void apply_effects(const ::quietcool::CoreEffects& effects,
                     ::quietcool::MonotonicMs now_ms);
  bool enqueue_effects(const ::quietcool::CoreEffects& effects,
                       ::quietcool::MonotonicMs now_ms);
  void apply_effect(const ::quietcool::CoreEffect& effect);
  void deliver_authority(const ::quietcool::AuthoritySnapshot& authority,
                         ::quietcool::MonotonicMs now_ms);
  void publish_last_tx_command(std::uint8_t byte);
  void flush_rx_counter_publications(::quietcool::MonotonicMs now_ms);
  void apply_burst_event(const ::quietcool::BurstEvent& event,
                         ::quietcool::MonotonicMs now_ms);
  void degrade(const char* reason);
  void note_budget_exhaustion();
  // Auto-grants permission_to_start_ from a confirmed authority snapshot
  // (item 3 of the permission-to-start design): fires only for the ONE
  // specific evidence source that means "the mandatory on-boot refresh, not
  // any later query, found the fan already running" — see the definition
  // site in quietcool_component.cpp for why BootQueryConsensus is the exact
  // and only trigger. A no-op once permission_to_start_ is already true, so
  // it is safe to call on every authority delivery rather than threading a
  // boot-only flag through deliver_authority's callers.
  void maybe_auto_grant_permission_to_start(
      const ::quietcool::AuthoritySnapshot& authority);
  // Publishes Last Confirmed Fan State and Fan Speed Capability from the
  // authority snapshot the PublishAuthorityEffect branch of apply_effect()
  // already carries — the same site every other AuthorityPublisher is fanned
  // out from, and the only site both fields' sources are confirmed-only.
  void publish_authority_diagnostics(
      const ::quietcool::AuthoritySnapshot& authority);
  // Publishes Remote Sender ID from provisioned_sender_. Called at exactly
  // the three sites provisioned_sender_ itself changes (see its declaration).
  void publish_remote_sender_id();

  EspMonotonicClock clock_;
  ::quietcool::Radio& radio_;
  EspHomeEventSink events_;
  EspHomePreferencesAdapter preferences_;
  ::quietcool::ConfirmationCore core_;
  ::quietcool::BurstTransmitter burst_;
  ::quietcool::CoreEffectDrain effect_drain_;
  ::quietcool::CoreCallbackQueue core_callbacks_;
  AuthorityPublisher* authority_publishers_[kMaxAuthorityPublishers]{};
  std::size_t authority_publisher_count_{0};
  // Terminal-degradation latch (M2, issue #9). Set before any degradation
  // publication so an on_value automation fired synchronously during degrade()
  // cannot re-enter the non-reentrant core through a public entry point.
  bool degraded_{false};
  // Effect-drain budget-exhaustion diagnostic bookkeeping (M1, issue #8). A
  // one-shot latch keeps a sustained storm from logging every loop.
  std::uint32_t last_budget_exhaustions_{0};
  bool budget_warning_active_{false};
  // Radio diagnostics (see radio_counters_test.cpp and the task-3 report).
  // rx_valid_count_ / rx_rejected_count_ are FRAME-VALIDATION counters — did
  // the frame decode against the provisioned sender — not classifier-
  // relevance counters: ConfirmationCore::on_frame's accept/reject verdict
  // is not recoverable from its returned CoreEffects without misclassifying
  // the common case (a still-accumulating consensus candidate legitimately
  // returns empty effects), and that verdict is core-private regardless.
  std::uint32_t tx_count_{0};
  std::uint32_t rx_valid_count_{0};
  std::uint32_t rx_rejected_count_{0};
  // Last TX Command's latch: set when a TransactionCommand burst is ACCEPTED,
  // published when the burst carrying the SAME token COMPLETES, and cleared on
  // that burst's rejection, fault, or revocation — so the diagnostic can never
  // name a byte that never went on the air (rounds 1 and 2).
  struct PendingTxCommand final {
    ::quietcool::TxToken token;
    std::uint8_t byte;
  };
  std::optional<PendingTxCommand> pending_tx_command_;
  // Change gate for Last TX Command (round 10): a fault storm repeats one
  // byte; identical values publish once.
  std::optional<std::uint8_t> published_tx_byte_;
  // RX counter entity updates are flushed from loop(), not per packet: a storm
  // (RF noise, or the provisioned remote retrying) otherwise produces one
  // native-API publish plus one synchronous on_value opportunity per packet
  // (round 2). The counters themselves stay exact; only entity updates are
  // paced, and the loop flush publishes the final total after a storm ends —
  // per-packet throttling alone left the shown value stale until the NEXT
  // packet, which may be days (round 2, all three engines).
  static constexpr ::quietcool::MonotonicMs kRxCounterPublishIntervalMs = 1000;
  ::quietcool::MonotonicMs last_rx_counter_publish_ms_{0};
  std::uint32_t published_rx_valid_count_{0};
  std::uint32_t published_rx_rejected_count_{0};
  // Last Valid RX Frame publishes only when the byte CHANGES: a retry storm
  // repeats one byte, so change-gating is the natural rate limit. The publish
  // itself is deferred until core_.on_frame has run (round 3, codex).
  std::optional<std::uint8_t> published_rx_frame_byte_;
  std::optional<std::uint8_t> pending_rx_frame_publish_;
  // Change gates for the two per-snapshot text diagnostics (round 3, opus,
  // measured): the post-drain channel fires every tick and TextSensor does
  // not dedupe.
  std::string published_last_confirmed_state_;
  std::string published_speed_capability_;
  // Mirrors ConfirmationCore's own sender_, so on_radio_packet can validate
  // an incoming frame the same way core eventually will, without reaching
  // into core. Updated at exactly the three sites where core's sender_
  // itself changes: restore() (setup()), Forget's EraseProvisioning, and
  // Learn's SaveProvisioning (see quietcool_component.cpp) — confirmed
  // against confirmation_core.cpp, where those are the only three sender_
  // assignments. Used by the RX counters above and by Remote Sender ID below.
  std::optional<::quietcool::SenderId> provisioned_sender_;
  // Restored diagnostics (Task 4). Last TX Command is published only for
  // TxReason::TransactionCommand (never a query burst) at the same
  // RequestTxBurst acceptance site that feeds the capability echo guard
  // (issue #31) — see confirmation_reducer.cpp's own_outbound_byte. Last
  // Valid RX Frame is published alongside rx_valid_count_. Neither byte is
  // latched as state here: each is read directly off the effect/packet that
  // is already in hand at its one publication site.
  text_sensor::TextSensor* last_tx_command_sensor_{nullptr};
  text_sensor::TextSensor* last_rx_frame_sensor_{nullptr};
  text_sensor::TextSensor* last_confirmed_state_sensor_{nullptr};
  text_sensor::TextSensor* speed_capability_sensor_{nullptr};
  text_sensor::TextSensor* remote_sender_id_sensor_{nullptr};
  sensor::Sensor* tx_count_sensor_{nullptr};
  sensor::Sensor* rx_valid_count_sensor_{nullptr};
  sensor::Sensor* rx_rejected_count_sensor_{nullptr};
  // Permission-to-start failsafe (see set_permission_to_start()'s
  // declaration above). Deliberately NOT restored from flash and NOT part of
  // RestorableState/preferences_: item 1 of the design requires false on
  // EVERY boot, with no exception for a graceful reboot, so there must be no
  // code path that could ever seed this true before either a fresh
  // BootQueryConsensus confirmation or an explicit HA grant happens. The
  // switch entity's own compiled restore_mode (ALWAYS_OFF, set in
  // switch.py) enforces the same rule at the entity layer, so the internal
  // flag and the HA-visible switch can never disagree about the boot
  // default.
  bool permission_to_start_{false};
  switch_::Switch* permission_switch_{nullptr};
  event::Event* start_refused_event_{nullptr};
};

}  // namespace esphome::quietcool
