/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/

#include "sample_sender_receiver.h"
#include "score/mw/com/impl/generic_proxy.h"
#include "score/mw/com/impl/generic_proxy_event.h"
#include "score/mw/com/impl/handle_type.h"

#include "score/concurrency/notification.h"

#include "score/mw/com/impl/proxy_event.h"
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <score/assert.hpp>
#include <score/hash.hpp>
#include <score/optional.hpp>

#include <cstring>   // for std::strncpy, std::strcmp
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <functional>

using namespace std::chrono_literals;

namespace score::mw::com
{

namespace
{

constexpr std::size_t START_HASH = 64738U;

std::ostream& operator<<(std::ostream& stream, const InstanceSpecifier& instance_specifier)
{
    stream << instance_specifier.ToString();
    return stream;
}

template <typename T>
void ToStringImpl(std::ostream& o, T t)
{
    o << t;
}

template <typename T, typename... Args>
void ToStringImpl(std::ostream& o, T t, Args... args)
{
    ToStringImpl(o, t);
    ToStringImpl(o, args...);
}

template <typename... Args>
std::string ToString(Args... args)
{
    std::ostringstream oss;
    ToStringImpl(oss, args...);
    return oss.str();
}

void HashArray(const std::array<LaneIdType, 16U>& array, std::size_t& seed)
{
    const std::ptrdiff_t buffer_size =
        reinterpret_cast<const std::uint8_t*>(&*array.cend()) - reinterpret_cast<const std::uint8_t*>(&*array.cbegin());
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD(buffer_size > 0);
    seed = score::cpp::hash_bytes_fnv1a(static_cast<const void*>(array.data()), static_cast<std::size_t>(buffer_size), seed);
}

// =============================================================================
// SampleReceiver — receives and validates samples from the skeleton
// =============================================================================
class SampleReceiver
{
  public:
    explicit SampleReceiver(const InstanceSpecifier& instance_specifier,
        std::function<void(std::uint32_t, const char*)> send_ack_callback,
        bool check_sample_hash = true)
        : instance_specifier_{instance_specifier},
          last_received_{},
          received_{0U},
          control_msgs_received_{0U},          // FIX: initialise in constructor too
          check_sample_hash_{check_sample_hash},
          send_ack_callback_{std::move(send_ack_callback)}
    {
    }

    // -------------------------------------------------------------------------
    // ReceiveSample — called for every sample delivered by GetNewSamples()
    // -------------------------------------------------------------------------
    void ReceiveSample(const MapApiLanesStamped& map) noexcept
    {
        // FIX: Check each control message with strcmp FIRST, then increment
        // counter and update phase. The previous version returned early BEFORE
        // updating the phase, so the proxy was stuck in WaitingForSync forever.

        // ── Control message: SYNC ───────────────────────────────────────────
        if (std::strcmp(map.sync_msg, "SYNC") == 0)
        {
            std::cout << "Received SYNC , sending ACK_SYNC\n";
            current_phase_ = Phase::Syncing;
            control_msgs_received_ += 1U;   // count it so the mismatch check works
            send_ack_callback_(0U, "ACK_SYNC");   // This tell skeleton we got SYNC
            return;
        }

        // ── Control message: SYNC_ACK ───────────────────────────────────────
        if (std::strcmp(map.sync_msg, "SYNC_ACK") == 0)
        {
            std::cout << "Received SYNC_ACK — data stream starting now\n";
            current_phase_ = Phase::ReceivingData;
            control_msgs_received_ += 1U;
            return;
        }

        // ── Control message: END ────────────────────────────────────────────
        if (std::strcmp(map.sync_msg, "END") == 0)
        {
            std::cout << "Received END — sender is done\n";
            current_phase_ = Phase::Done;
            control_msgs_received_ += 1U;
            return;
        }

        // ── Real data sample ────────────────────────────────────────────────
        // Guard: ignore data that arrives before the handshake is complete.
        if (current_phase_ != Phase::ReceivingData)
        {
            std::cerr << "Warning: received data sample before SYNC_ACK, ignoring\n";
            return;
        }

        std::cout << "Received data sample: x=" << map.x << "\n";
        if (CheckReceivedSample(map))
        {
            received_ += 1U;
            send_ack_callback_(map.x, "ACK");   // This tell skeleton we got this data
        }
        last_received_ = map.x;
    }

    // ── Accessors ────────────────────────────────────────────────────────────

    /// Number of valid real-data samples received (SYNC/ACK/END not counted).
    std::size_t GetReceivedSampleCount() const noexcept { return received_; }

    /// Number of control messages received (SYNC, SYNC_ACK, END).
    /// FIX: used by RunAsProxy to build the correct total for the mismatch check.
    std::size_t GetControlMsgCount() const noexcept { return control_msgs_received_; }

    /// True once the END message has been received.
    bool IsDone() const noexcept { return current_phase_ == Phase::Done; }

  private:

    // ── Phase state machine ──────────────────────────────────────────────────
    enum class Phase
    {
        WaitingForSync,   // initial state — no SYNC seen yet
        Syncing,          // SYNC received, waiting for SYNC_ACK
        ReceivingData,    // SYNC_ACK received — real data accepted
        Done              // END received
    };

    Phase current_phase_{Phase::WaitingForSync};

    // ── Sample integrity checks ──────────────────────────────────────────────
    bool CheckReceivedSample(const MapApiLanesStamped& map) const noexcept
    {
        // Order check: x must always be strictly increasing
        if (last_received_.has_value())
        {
            if (map.x <= last_received_.value())
            {
                std::cerr << ToString(instance_specifier_,
                                      ": The received sample is out of order. Expected that ",
                                      map.x,
                                      " > ",
                                      last_received_.value(),
                                      "\n");
                return false;
            }
        }

        // Hash check: recompute hash from lane data and compare with sent value
        if (check_sample_hash_)
        {
            std::size_t hash_value = START_HASH;
            for (const MapApiLaneData& lane : map.lanes)
            {
                HashArray(lane.successor_lanes, hash_value);
            }

            if (hash_value != map.hash_value)
            {
                std::cerr << ToString(instance_specifier_,
                                      ": Unexpected data received, hash comparison failed: ",
                                      hash_value,
                                      ", expected ",
                                      map.hash_value,
                                      "\n");
                return false;
            }
        }

        return true;
    }

    // ── Member variables ─────────────────────────────────────────────────────
    const score::mw::com::InstanceSpecifier& instance_specifier_;
    score::cpp::optional<std::uint32_t> last_received_;
    std::size_t received_;
    std::size_t control_msgs_received_;
    bool check_sample_hash_;
    std::function<void(std::uint32_t, const char*)> send_ack_callback_;
};

// =============================================================================
// Helper: get the typed proxy event from an IpcBridgeProxy
// =============================================================================
score::cpp::optional<std::reference_wrapper<impl::ProxyEvent<MapApiLanesStamped>>>
GetMapApiLanesStampedProxyEvent(IpcBridgeProxy& proxy)
{
    return proxy.map_api_lanes_stamped_;
}

// =============================================================================
// Helper: get the generic proxy event from a GenericProxy
// =============================================================================
score::cpp::optional<std::reference_wrapper<impl::GenericProxyEvent>>
GetMapApiLanesStampedProxyEvent(GenericProxy& generic_proxy)
{
    const std::string event_name{"map_api_lanes_stamped"};
    auto event_it = generic_proxy.GetEvents().find(event_name);
    if (event_it == generic_proxy.GetEvents().cend())
    {
        std::cerr << "Could not find event " << event_name << " in generic proxy event map\n";
        return {};
    }
    return event_it->second;
}

/// Returns the value pointed to by a typed pointer.
const MapApiLanesStamped& GetSamplePtrValue(const MapApiLanesStamped* const sample_ptr)
{
    return *sample_ptr;
}

/// Casts a void pointer to MapApiLanesStamped and returns the value.
const MapApiLanesStamped& GetSamplePtrValue(const void* const void_ptr)
{
    auto* const typed_ptr = static_cast<const MapApiLanesStamped*>(void_ptr);
    return *typed_ptr;
}

/// Extracts a non-const pointer from a SamplePtr<T>. Used only in death tests.
template <typename SampleType>
SampleType* ExtractNonConstPointer(const SamplePtr<SampleType>& sample) noexcept
{
    const SampleType* sample_const_ptr = sample.get();
    // See original comment: we are casting away constness of the pointer, not
    // of the underlying object, so this is not UB — the object itself is not const.
    auto* sample_non_const_ptr = const_cast<SampleType*>(sample_const_ptr);
    return sample_non_const_ptr;
}

void ModifySampleValue(const SamplePtr<MapApiLanesStamped>& sample)
{
    auto* const sample_non_const_ptr = ExtractNonConstPointer(sample);
    sample_non_const_ptr->x += 1;
}

void ModifySampleValue(const SamplePtr<void>& sample)
{
    auto* const sample_non_const_ptr = ExtractNonConstPointer(sample);
    auto* const typed_ptr = static_cast<MapApiLanesStamped*>(sample_non_const_ptr);
    typed_ptr->x += 1;
}

// =============================================================================
// Helper: poll FindService until a handle is found
// =============================================================================
template <typename ProxyType = IpcBridgeProxy>
score::Result<impl::HandleType> GetHandleFromSpecifier(const InstanceSpecifier& instance_specifier)
{
    std::cout << ToString(instance_specifier, ": Running as proxy, looking for services\n");
    ServiceHandleContainer<impl::HandleType> handles{};
    do
    {
        auto handles_result = ProxyType::FindService(instance_specifier);
        if (!handles_result.has_value())
        {
            return MakeUnexpected<impl::HandleType>(std::move(handles_result.error()));
        }
        handles = std::move(handles_result).value();
        if (handles.size() == 0)
        {
            std::this_thread::sleep_for(500ms);
        }
    } while (handles.size() == 0);

    std::cout << ToString(instance_specifier, ": Found service, instantiating proxy\n");
    return handles.front();
}

// =============================================================================
// PrepareMapLaneSample — allocates and fills one sample
//
// Parameters:
//   skeleton   — the skeleton used to allocate from shared memory
//   cycle      — the cycle counter, stored in sample->x
//   sync_msg   — a C-string written into the fixed char[16] field.
//                Pass "" for real data samples, or "SYNC"/"SYNC_ACK"/"END"
//                for control messages.
// =============================================================================
Result<SampleAllocateePtr<MapApiLanesStamped>> PrepareMapLaneSample(IpcBridgeSkeleton& skeleton,
                                                                    const std::size_t cycle,
                                                                    const char* sync_msg)   // FIX: use const char* (simpler, compatible with string literals)
{
    const std::default_random_engine::result_type seed{static_cast<std::default_random_engine::result_type>(
        std::chrono::steady_clock::now().time_since_epoch().count())};
    std::default_random_engine rng{seed};

    auto sample_result = skeleton.map_api_lanes_stamped_.Allocate();
    if (!sample_result.has_value())
    {
        return sample_result;
    }
    auto sample = std::move(sample_result).value();

    sample->hash_value = START_HASH;
    sample->x = static_cast<std::uint32_t>(cycle);

    // FIX: use strncpy to copy into the fixed-size char array.
    // Cannot use = assignment on char arrays in C++.
    std::strncpy(sample->sync_msg, sync_msg, 15);  // copy at most 15 characters
    sample->sync_msg[15] = '\0';                    // always null-terminate

    std::cout << ToString("Sending sample: x=", sample->x,
                          ", sync_msg=\"", sample->sync_msg, "\"\n");

    for (MapApiLaneData& lane : sample->lanes)
    {
        for (LaneIdType& successor : lane.successor_lanes)
        {
            successor = std::uniform_int_distribution<std::size_t>()(rng);
        }
        HashArray(lane.successor_lanes, sample->hash_value);
    }

    return sample;
}

}  // namespace

// =============================================================================
// RunAsProxy — receives data published by the skeleton
// =============================================================================
template <typename ProxyType, typename ProxyEventType>
int EventSenderReceiver::RunAsProxy(const score::mw::com::InstanceSpecifier& instance_specifier,
                                    const score::cpp::optional<std::chrono::milliseconds> cycle_time,
                                    const std::size_t num_cycles,
                                    bool try_writing_to_data_segment,
                                    bool check_sample_hash)
{
    // For GenericProxy, SampleType is void. For IpcBridgeProxy it is MapApiLanesStamped.
    using SampleType =
        typename std::conditional<std::is_same<ProxyType, GenericProxy>::value, void, MapApiLanesStamped>::type;

    constexpr std::size_t SAMPLES_PER_CYCLE = 2U;

    // ── Find the service ─────────────────────────────────────────────────────
    auto handle_result = GetHandleFromSpecifier(instance_specifier);
    if (!handle_result.has_value())
    {
        std::cerr << "Unable to find service: " << instance_specifier
                  << ". Failed with error: " << handle_result.error() << ", bailing!\n";
        return EXIT_FAILURE;
    }
    auto handle = handle_result.value();

    // ── Create proxy ─────────────────────────────────────────────────────────
    auto proxy_result = ProxyType::Create(std::move(handle));
    if (!proxy_result.has_value())
    {
        std::cerr << "Unable to construct proxy: " << proxy_result.error() << ", bailing!\n";
        return EXIT_FAILURE;
    }
    auto& proxy = proxy_result.value();

    // ── Get the event handle ─────────────────────────────────────────────────
    auto map_api_lanes_stamped_event_optional = GetMapApiLanesStampedProxyEvent(proxy);
    if (!map_api_lanes_stamped_event_optional.has_value())
    {
        std::cerr << "Could not get MapApiLanesStamped proxy event\n";
        return EXIT_FAILURE;
    }
    auto& map_api_lanes_stamped_event = map_api_lanes_stamped_event_optional.value().get();

    // ── Optional callback mode (no cycle-time polling) ───────────────────────
    concurrency::Notification event_received;
    if (!cycle_time.has_value())
    {
        map_api_lanes_stamped_event.SetReceiveHandler([&event_received, &instance_specifier]() {
            std::cout << ToString(instance_specifier, ": Callback called\n");
            event_received.notify();
        });
    }

    // ── Subscribe ────────────────────────────────────────────────────────────
    std::cout << ToString(instance_specifier, ": Subscribing to service\n");
    map_api_lanes_stamped_event.Subscribe(SAMPLES_PER_CYCLE);

    // Start the ACK skeleton in a background thread

    // These variables are shared between the main loop and the ACK thread:
    std::mutex           ack_send_mutex{};
    std::condition_variable ack_send_cv{};
    std::uint32_t        cycle_to_ack{0U};
    bool                 ack_requested{false};
    bool                 ack_thread_should_stop{false};
    char                    ack_status[16]{};

    //SampleReceiver receiver{instance_specifier, check_sample_hash};
    SampleReceiver receiver{instance_specifier,
                            [&](std::uint32_t cycle_num, const char *status) {
                              // This lambda is called by ReceiveSample whenever
                              // an ACK needs to be sent. It fills the shared
                              // variables and wakes up the ACK thread.
                              std::lock_guard<std::mutex> lock{ack_send_mutex};
                              cycle_to_ack = cycle_num;
                              std::strncpy(ack_status, status, 15);
                              ack_status[15] = '\0';
                              ack_requested = true;
                              ack_send_cv.notify_one();
                            },
                            check_sample_hash
    };

    // The ACK-sending thread:
    std::thread ack_thread([&]()
    {
        // Create the ACK skeleton (proxy sends ACK, so it needs a skeleton here)
        const auto ack_instance_result =
            score::mw::com::InstanceSpecifier::Create(std::string{"score/AckMessage"});
        if (!ack_instance_result.has_value())
        {
            std::cerr << "ACK: invalid instance specifier\n";
            return;
        }

        auto ack_skeleton_result = AckBridgeSkeleton::Create(ack_instance_result.value());
        if (!ack_skeleton_result.has_value())
        {
            std::cerr << "ACK: could not create skeleton: " << ack_skeleton_result.error() << "\n";
            return;
        }
        auto &ack_skeleton = ack_skeleton_result.value();

        const auto ack_offer_result = ack_skeleton.OfferService();
        if (!ack_offer_result.has_value()) {
          std::cerr << "ACK: could not offer service: "
                    << ack_offer_result.error() << "\n";
          return;
        }
        // Loop: wait for a signal, then send ACK
        while (true)
        {
            std::uint32_t cycle_num{0U};
            char status_to_send[16]{};
            {
                std::unique_lock lock{ack_send_mutex};
                // Sleep until main loop signals us or tells us to stop
                ack_send_cv.wait(lock, [&]{
                    return ack_requested || ack_thread_should_stop;
                });

                if (ack_thread_should_stop) { break; }

                cycle_num = cycle_to_ack;
                std::strncpy(status_to_send, ack_status, 15);
                ack_requested = false;   // reset for next cycle
            }

            // Build and send the ACK sample
            auto ack_sample_result = ack_skeleton.ack_event_.Allocate();
            if (!ack_sample_result.has_value()) { continue; }

            auto ack_sample = std::move(ack_sample_result).value();
            ack_sample->ack_for_cycle = cycle_num;
            std::strncpy(ack_sample->status, status_to_send, 15);
            ack_sample->status[15] = '\0';

            ack_skeleton.ack_event_.Send(std::move(ack_sample));
            std::cout << "Sent ACK for cycle " << cycle_num << "\n";
        }

        ack_skeleton.StopOfferService();
    });
    // ── Main receive loop ────────────────────────────────────────────────────
    // We loop until we have received num_cycles *real data* samples.
    // Control messages (SYNC, SYNC_ACK, END) do NOT advance the cycle counter.
    for (std::size_t cycle = 0U; cycle < num_cycles;)
    {
        const auto cycle_start_time = std::chrono::steady_clock::now();

        if (cycle_time.has_value())
        {
            std::this_thread::sleep_for(*cycle_time);
        }

        // FIX: snapshot BOTH counters before calling GetNewSamples so we can
        // compute exactly how many of each kind were processed this iteration.
        const auto received_before      = receiver.GetReceivedSampleCount();
        const auto control_before       = receiver.GetControlMsgCount();

        Result<std::size_t> num_samples_received = map_api_lanes_stamped_event.GetNewSamples(
            [&receiver, try_writing_to_data_segment](SamplePtr<SampleType> sample) noexcept {
                if (try_writing_to_data_segment)
                {
                    // Death-test path: attempt to write into read-only shared memory.
                    ModifySampleValue(sample);
                }
                const MapApiLanesStamped& sample_value = GetSamplePtrValue(sample.get());
                receiver.ReceiveSample(sample_value);
            },
            SAMPLES_PER_CYCLE);

        // FIX: total_processed = real data + control messages delivered this cycle.
        // This must equal what GetNewSamples() reported, otherwise something went wrong.
        const auto total_processed =
            (receiver.GetReceivedSampleCount() + receiver.GetControlMsgCount())
            - (received_before + control_before);

        // ── Error detection ──────────────────────────────────────────────────
        const bool get_new_samples_api_error = !num_samples_received.has_value();

        // FIX: renamed to 'mismatch' — single, unambiguous variable name.
        const bool mismatch = (*num_samples_received != total_processed);

        const bool receive_handler_called_without_new_samples =
            (*num_samples_received == 0U) && !cycle_time.has_value();

        if (get_new_samples_api_error || mismatch || receive_handler_called_without_new_samples)
        {
            std::stringstream ss;
            ss << instance_specifier << ": Error in cycle " << cycle << " during sample reception: ";

            if (!get_new_samples_api_error)
            {
                if (mismatch)
                {
                    ss << "number of processed samples doesn't match what IPC claims: "
                       << *num_samples_received << " vs " << total_processed;
                }
                else
                {
                    ss << "expected at least one new sample since event-notifier was called, "
                          "but GetNewSamples() provided none!";
                }
            }
            else
            {
                ss << std::move(num_samples_received).error();
            }
            ss << ", terminating.\n";
            std::cerr << ss.str();

            map_api_lanes_stamped_event.Unsubscribe();
            return EXIT_FAILURE;
        }

        // FIX: advance cycle only by the number of *real data* samples received,
        // not by num_samples_received which includes control messages.
        const auto real_data_received = receiver.GetReceivedSampleCount() - received_before;
        if (real_data_received >= 1U)
        {
            std::cout << ToString(instance_specifier, ": Proxy received valid data\n");
            ////Signal the ACK thread to send ACK for this cycle
            //{
            //    std::lock_guard lock{ack_send_mutex};
            //    cycle_to_ack = static_cast<std::uint32_t>(cycle);
            //    ack_requested = true;
            //}
            //ack_send_cv.notify_one(); //wake up the ACK thread
            cycle += real_data_received;
        }

        const auto cycle_duration = std::chrono::steady_clock::now() - cycle_start_time;
        std::cout << ToString(instance_specifier,
                              ": Cycle duration ",
                              std::chrono::duration_cast<std::chrono::milliseconds>(cycle_duration).count(),
                              "ms\n");

        event_received.reset();
    }
    // Tell the ACK thread to stop and wait for it to finish
    {
        std::lock_guard lock{ack_send_mutex};
        ack_thread_should_stop = true;
    }
    ack_send_cv.notify_one();
    ack_thread.join();   // wait until thread has fully exited

    std::cout << ToString(instance_specifier, ": Unsubscribing...\n");
    map_api_lanes_stamped_event.Unsubscribe();
    std::cout << ToString(instance_specifier, ": and terminating, bye bye\n");
    return EXIT_SUCCESS;
}

// =============================================================================
// RunAsSkeleton — sends data to all subscribed proxies
// =============================================================================
int EventSenderReceiver::RunAsSkeleton(const score::mw::com::InstanceSpecifier& instance_specifier,
                                       const std::chrono::milliseconds cycle_time,
                                       const std::size_t num_cycles)
{
    // ── Create skeleton ──────────────────────────────────────────────────────
    auto create_result = IpcBridgeSkeleton::Create(instance_specifier);
    if (!create_result.has_value())
    {
        std::cerr << "Unable to construct skeleton: " << create_result.error() << ", bailing!\n";
        return EXIT_FAILURE;
    }
    auto& skeleton = create_result.value();

    // ── Offer the service ────────────────────────────────────────────────────
    const auto offer_result = skeleton.OfferService();
    if (!offer_result.has_value())
    {
        std::cerr << "Unable to offer service for skeleton: " << offer_result.error() << ", bailing!\n";
        return EXIT_FAILURE;
    }

    // ── Start ACK-listening proxy in background thread ───────────────────────
    std::thread ack_listener_thread([this]()
    {
        const auto ack_instance_result =
            score::mw::com::InstanceSpecifier::Create(std::string{"score/AckMessage"});
        if (!ack_instance_result.has_value()) { return; }

        // Find the ACK service (offered by the proxy process)
        ServiceHandleContainer<impl::HandleType> handles{};
        do
        {
            auto result = AckBridgeProxy::FindService(ack_instance_result.value());
            if (!result.has_value()) { return; }
            handles = std::move(result).value();
            if (handles.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); }
        } while (handles.empty());

        auto ack_proxy_result = AckBridgeProxy::Create(handles.front());
        if (!ack_proxy_result.has_value()) { return; }
        auto& ack_proxy = ack_proxy_result.value();

        ack_proxy.ack_event_.Subscribe(2U);

        // Keep reading ACKs and storing them
        while (true)
        {
            ack_proxy.ack_event_.GetNewSamples(
                [this](SamplePtr<AckMessage> sample) noexcept
                {
                    const AckMessage& ack = *sample.get();
                    if (std::strcmp(ack.status, "ACK_SYNC") == 0) {
                      // Proxy confirmed it received SYNC — wake up the SYNC
                      // waiting loop
                      std::lock_guard<std::mutex> lock{ack_mutex_};
                      sync_acknowledged_ = true;
                      ack_received_cv_.notify_one();
                    } else if (std::strcmp(ack.status, "ACK") == 0) {
                      // Store the ACK and wake up RunAsSkeleton
                      std::lock_guard lock{ack_mutex_};
                      last_ack_cycle_ = ack.ack_for_cycle;
                      ack_received_cv_
                        .notify_one(); // wake up the skeleton loop
                    }
                },
                2U);

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    ack_listener_thread.detach();   // runs independently, skeleton doesn't wait for it

    // ── PHASE 1: Send SYNC repeatedly until a proxy has subscribed ───────────
    // This avoids the race condition where SYNC is sent before the proxy is
    // ready to receive it, which would leave the proxy stuck in WaitingForSync.
    // NEW: keep sending SYNC every 500ms until proxy sends back ACK_SYNC
    std::cout << "Waiting for proxy, sending SYNC...\n";
    while (true) {
      // Send one SYNC
      auto sync_sample = PrepareMapLaneSample(skeleton, 0U, "SYNC");
      if (!sync_sample.has_value()) {
        return EXIT_FAILURE;
      }
      {
        std::lock_guard<std::mutex> lock{event_sending_mutex_};
        skeleton.map_api_lanes_stamped_.Send(std::move(sync_sample).value());
      }

      // Wait up to 500ms for ACK_SYNC from the proxy
      std::unique_lock<std::mutex> lock{ack_mutex_};
      const bool got_ack_sync =
          ack_received_cv_.wait_for(lock, std::chrono::milliseconds(500),
                                    [this] {
                                      return sync_acknowledged_;
                                    } // stop waiting if ACK_SYNC arrived
          );

      if (got_ack_sync) {
        std::cout << "Proxy acknowledged SYNC — proceeding to SYNC_ACK\n";
        break; // exit the loop, proxy is ready
      }
      // else: timeout expired, no ACK_SYNC yet → loop again and send another
      // SYNC
      std::cout << "No ACK_SYNC yet, retrying...\n";
    }

    // Give the proxy a moment to process the last SYNC before sending SYNC_ACK
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ── PHASE 2: Send SYNC_ACK — signals that real data is about to start ────
    std::cout << "Sending SYNC_ACK...\n";
    auto sync_ack_sample = PrepareMapLaneSample(skeleton, 0U, "SYNC_ACK");
    if (!sync_ack_sample.has_value()) { return EXIT_FAILURE; }
    {
        std::lock_guard lock{event_sending_mutex_};
        skeleton.map_api_lanes_stamped_.Send(std::move(sync_ack_sample).value());
    }

    // Small pause so the proxy transitions to ReceivingData before data arrives
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ── PHASE 3: Send real data ──────────────────────────────────────────────
    std::cout << "Starting to send data\n";
    for (std::size_t cycle = 0U; cycle < num_cycles || num_cycles == 0U; ++cycle)
    {
        // Empty sync_msg ("") means this is a real data sample
        auto sample_result = PrepareMapLaneSample(skeleton, cycle, "");
        if (!sample_result.has_value())
        {
            std::cerr << "Sample allocation failed. Exiting.\n";
            return EXIT_FAILURE;
        }
        {
            std::lock_guard lock{event_sending_mutex_};
            skeleton.map_api_lanes_stamped_.Send(std::move(sample_result).value());
            event_published_ = true;
        }
        //Wait for ACK form the proxy
        //We sleep here until the ACK-listening thread wakes us up.
        //The timeout (5s) prevents waiting forever if the proxy crashes.

        {
            std::unique_lock lock{ack_mutex_};

            const bool ack_arrived = ack_received_cv_.wait_for(
                lock,
                std::chrono::seconds(5),
                [this, cycle]{
                    //wake up condition: last_ack_cycle_ must match current cycle
                    return last_ack_cycle_.has_value()&&
                            last_ack_cycle_.value() == static_cast<std::uint32_t>(cycle);
                }
            );
            if(!ack_arrived){
                std::cerr << "Timeout: no ACK received for cycle "<< cycle << ", terminating.\n";
                return EXIT_FAILURE;
            }

            std::cout << "ACK received for cycle " << cycle << ", proceeding.\n";
        }
        //only sleep between cycles if desired - now we're ACK-gated not time-gated
        std::this_thread::sleep_for(cycle_time);
    }

    // ── PHASE 4: Send END — signals that no more data will follow ────────────
    std::cout << "Sending END...\n";
    auto end_sample = PrepareMapLaneSample(skeleton, 0U, "END");
    if (!end_sample.has_value()) { return EXIT_FAILURE; }
    {
        std::lock_guard lock{event_sending_mutex_};
        skeleton.map_api_lanes_stamped_.Send(std::move(end_sample).value());
    }

    // ── Stop offering the service ─────────────────────────────────────────────
    std::cout << "Stop offering service...";
    skeleton.StopOfferService();
    std::cout << " and terminating, bye bye\n";

    return EXIT_SUCCESS;
}

// =============================================================================
// Explicit template instantiations
// (required because the template body is in the .cpp file)
// =============================================================================
template int EventSenderReceiver::RunAsProxy<IpcBridgeProxy, impl::ProxyEvent<MapApiLanesStamped>>(
    const score::mw::com::InstanceSpecifier&,
    const score::cpp::optional<std::chrono::milliseconds>,
    const std::size_t,
    bool,
    bool);

template int EventSenderReceiver::RunAsProxy<impl::GenericProxy, impl::GenericProxyEvent>(
    const score::mw::com::InstanceSpecifier&,
    const score::cpp::optional<std::chrono::milliseconds>,
    const std::size_t,
    bool,
    bool);

}  // namespace score::mw::com
