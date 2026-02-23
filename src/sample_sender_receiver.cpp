/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/
#include "sample_sender_receiver.h"
#include "score/mw/com/impl/generic_proxy.h"
#include "score/mw/com/impl/generic_proxy_event.h"
#include "score/mw/com/impl/handle_type.h"
#include "score/concurrency/notification.h"
#include "score/mw/com/impl/proxy_event.h"
#include <score/assert.hpp>
#include <score/hash.hpp>
#include <score/optional.hpp>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

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
void ToStringImpl(std::ostream& o, T t) { o << t; }

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
        reinterpret_cast<const std::uint8_t*>(&*array.cend()) -
        reinterpret_cast<const std::uint8_t*>(&*array.cbegin());
    SCORE_LANGUAGE_FUTURECPP_ASSERT_PRD(buffer_size > 0);
    seed = score::cpp::hash_bytes_fnv1a(
        static_cast<const void*>(array.data()),
        static_cast<std::size_t>(buffer_size),
        seed);
}

class SampleReceiver
{
  public:
    explicit SampleReceiver(const InstanceSpecifier& instance_specifier,
                            const std::string& channel_label,
                            bool check_sample_hash = true)
        : instance_specifier_{instance_specifier},
          channel_label_{channel_label},
          last_received_{},
          received_{0U},
          check_sample_hash_{check_sample_hash}
    {
    }

    void ReceiveSample(const MapApiLanesStamped& map) noexcept
    {
        std::cout << ToString(instance_specifier_, " [", channel_label_, "]: Received sample: ", map.x, "\n");
        if (CheckReceivedSample(map))
        {
            received_ += 1U;
        }
        last_received_ = map.x;
    }

    std::size_t GetReceivedSampleCount() const noexcept { return received_; }

  private:
    bool CheckReceivedSample(const MapApiLanesStamped& map) const noexcept
    {
        if (last_received_.has_value() && map.x <= last_received_.value())
        {
            std::cerr << ToString(instance_specifier_,
                                  " [", channel_label_, "]: Out-of-order sample: ",
                                  map.x, " <= ", last_received_.value(), "\n");
            return false;
        }
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
                                      " [", channel_label_, "]: Hash mismatch: ",
                                      hash_value, " vs expected ", map.hash_value, "\n");
                return false;
            }
        }
        return true;
    }

    const score::mw::com::InstanceSpecifier& instance_specifier_;
    const std::string channel_label_;
    score::cpp::optional<std::uint32_t> last_received_;
    std::size_t received_;
    bool check_sample_hash_;
};

score::Result<impl::HandleType> GetHandleFromSpecifier(const InstanceSpecifier& instance_specifier)
{
    std::cout << ToString(instance_specifier, ": Looking for service\n");
    ServiceHandleContainer<impl::HandleType> handles{};
    do
    {
        auto handles_result = IpcBridgeProxy::FindService(instance_specifier);
        if (!handles_result.has_value())
        {
            return MakeUnexpected<impl::HandleType>(std::move(handles_result.error()));
        }
        handles = std::move(handles_result).value();
        if (handles.empty())
        {
            std::this_thread::sleep_for(500ms);
        }
    } while (handles.empty());

    std::cout << ToString(instance_specifier, ": Found service, instantiating proxy\n");
    return handles.front();
}

template <typename SkeletonOrEvent>
Result<SampleAllocateePtr<MapApiLanesStamped>> PrepareSample(SkeletonOrEvent& event,
                                                              const std::size_t cycle,
                                                              const std::string& label)
{
    const std::default_random_engine::result_type seed{
        static_cast<std::default_random_engine::result_type>(
            std::chrono::steady_clock::now().time_since_epoch().count())};
    std::default_random_engine rng{seed};

    auto sample_result = event.Allocate();
    if (!sample_result.has_value())
    {
        return sample_result;
    }
    auto sample = std::move(sample_result).value();
    sample->hash_value = START_HASH;
    sample->x = static_cast<std::uint32_t>(cycle);
    std::cout << ToString("[", label, "] Sending sample: ", sample->x, "\n");

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
// RunAsSkeleton
// Role: sends on instance_specifier, receives on reverse_specifier
// =============================================================================
int EventSenderReceiver::RunAsSkeleton(const score::mw::com::InstanceSpecifier& instance_specifier,
                                       const score::mw::com::InstanceSpecifier& reverse_specifier,
                                       const std::chrono::milliseconds cycle_time,
                                       const std::size_t num_cycles)
{
    // Create the forward skeleton (this process provides the main service)
    auto create_result = IpcBridgeSkeleton::Create(instance_specifier);
    if (!create_result.has_value())
    {
        std::cerr << "Unable to construct skeleton: " << create_result.error() << ", bailing!\n";
        return EXIT_FAILURE;
    }
    auto& skeleton = create_result.value();
    const auto offer_result = skeleton.OfferService();
    if (!offer_result.has_value())
    {
        std::cerr << "Unable to offer service: " << offer_result.error() << ", bailing!\n";
        return EXIT_FAILURE;
    }

    // Create reverse proxy to receive what the proxy sends back
    auto rev_handle_result = GetHandleFromSpecifier(reverse_specifier);
    if (!rev_handle_result.has_value())
    {
        std::cerr << "Skeleton: Unable to find reverse service: "
                  << rev_handle_result.error() << ", bailing!\n";
        return EXIT_FAILURE;
    }
    auto rev_proxy_result = IpcBridgeProxy::Create(std::move(rev_handle_result).value());
    if (!rev_proxy_result.has_value())
    {
        std::cerr << "Skeleton: Unable to create reverse proxy: "
                  << rev_proxy_result.error() << ", bailing!\n";
        return EXIT_FAILURE;
    }
    auto& rev_proxy = rev_proxy_result.value();

    constexpr std::size_t SAMPLES_PER_CYCLE = 2U;
    rev_proxy.map_api_lanes_stamped_.Subscribe(SAMPLES_PER_CYCLE);
    std::cout << "Skeleton: subscribed to reverse channel\n";

    SampleReceiver reverse_receiver{reverse_specifier, "skeleton<-proxy", true};

    std::cout << "Starting to send data (bidirectional mode)\n";
    for (std::size_t cycle = 0U; cycle < num_cycles || num_cycles == 0U; ++cycle)
    {
        // Send forward sample
        auto sample_result = PrepareSample(skeleton.map_api_lanes_stamped_, cycle, "skeleton->proxy");
        if (!sample_result.has_value())
        {
            std::cerr << "Skeleton: Failed to allocate sample. Exiting.\n";
            return EXIT_FAILURE;
        }
        {
            std::lock_guard lock{event_sending_mutex_};
            skeleton.map_api_lanes_stamped_.Send(std::move(sample_result).value());
            event_published_ = true;
        }

        // Poll reverse channel for feedback from proxy
        rev_proxy.map_api_lanes_stamped_.GetNewSamples(
            [&reverse_receiver](SamplePtr<MapApiLanesStamped> sample) noexcept {
                reverse_receiver.ReceiveSample(*sample);
            },
            SAMPLES_PER_CYCLE);

        std::this_thread::sleep_for(cycle_time);
    }

    std::cout << "Stop offering service...";
    rev_proxy.map_api_lanes_stamped_.Unsubscribe();
    skeleton.StopOfferService();
    std::cout << "and terminating, bye bye\n";
    return EXIT_SUCCESS;
}

// =============================================================================
// RunAsProxy (template)
// Role: receives on instance_specifier, sends back on reverse_specifier
// =============================================================================
template <typename ProxyType, typename ProxyEventType>
int EventSenderReceiver::RunAsProxy(const score::mw::com::InstanceSpecifier& instance_specifier,
                                    const score::mw::com::InstanceSpecifier& reverse_specifier,
                                    const score::cpp::optional<std::chrono::milliseconds> cycle_time,
                                    const std::size_t num_cycles,
                                    bool try_writing_to_data_segment,
                                    bool check_sample_hash)
{
    constexpr std::size_t SAMPLES_PER_CYCLE = 2U;
    constexpr bool is_generic = std::is_same<ProxyType, GenericProxy>::value;

    // Find and create the forward-receiving proxy
    auto handle_result = GetHandleFromSpecifier(instance_specifier);
    if (!handle_result.has_value())
    {
        std::cerr << "Unable to find service: " << instance_specifier
                  << ". Error: " << handle_result.error() << ", bailing!\n";
        return EXIT_FAILURE;
    }

    auto proxy_result = ProxyType::Create(std::move(handle_result).value());
    if (!proxy_result.has_value())
    {
        std::cerr << "Unable to construct proxy: " << proxy_result.error() << ", bailing!\n";
        return EXIT_FAILURE;
    }
    auto& proxy = proxy_result.value();

    // Typed path only: create reverse skeleton on reverse_specifier to send feedback back
    score::cpp::optional<IpcBridgeSkeleton> reverse_skeleton_opt{};
    if constexpr (!is_generic)
    {
        auto sk_result = IpcBridgeSkeleton::Create(reverse_specifier);
        if (!sk_result.has_value())
        {
            std::cerr << "Proxy: Unable to create reverse skeleton: "
                      << sk_result.error() << ", bailing!\n";
            return EXIT_FAILURE;
        }
        reverse_skeleton_opt = std::move(sk_result).value();
        const auto offer = reverse_skeleton_opt->OfferService();
        if (!offer.has_value())
        {
            std::cerr << "Proxy: Unable to offer reverse skeleton: "
                      << offer.error() << ", bailing!\n";
            return EXIT_FAILURE;
        }
        std::cout << ToString(instance_specifier, ": Reverse skeleton ready\n");
    }

    // GenericProxy path — receive only, no reverse send
    if constexpr (is_generic)
    {
        const std::string event_name{"map_api_lanes_stamped"};
        auto event_it = proxy.GetEvents().find(event_name);
        if (event_it == proxy.GetEvents().cend())
        {
            std::cerr << "Could not find event " << event_name << " in generic proxy\n";
            return EXIT_FAILURE;
        }
        auto& forward_event = event_it->second;

        concurrency::Notification event_received;
        if (!cycle_time.has_value())
        {
            forward_event.SetReceiveHandler([&event_received, &instance_specifier]() {
                std::cout << ToString(instance_specifier, ": Callback called\n");
                event_received.notify();
            });
        }

        std::cout << ToString(instance_specifier, ": Subscribing to map_api_lanes_stamped\n");
        forward_event.Subscribe(SAMPLES_PER_CYCLE);

        SampleReceiver forward_receiver{instance_specifier, "proxy<-skeleton", check_sample_hash};

        for (std::size_t cycle = 0U; cycle < num_cycles;)
        {
            const auto cycle_start = std::chrono::steady_clock::now();
            if (cycle_time.has_value()) { std::this_thread::sleep_for(*cycle_time); }

            const auto received_before = forward_receiver.GetReceivedSampleCount();

            Result<std::size_t> num_received = forward_event.GetNewSamples(
                [&](SamplePtr<void> sample) noexcept {
                    if (try_writing_to_data_segment)
                    {
                        auto* p = const_cast<MapApiLanesStamped*>(
                            static_cast<const MapApiLanesStamped*>(sample.get()));
                        p->x += 1;
                    }
                    const auto* typed = static_cast<const MapApiLanesStamped*>(sample.get());
                    forward_receiver.ReceiveSample(*typed);
                },
                SAMPLES_PER_CYCLE);

            const auto received = forward_receiver.GetReceivedSampleCount() - received_before;

            if (!num_received.has_value() ||
                *num_received != received ||
                (*num_received == 0 && !cycle_time.has_value()))
            {
                std::stringstream ss;
                ss << instance_specifier << ": Error in cycle " << cycle << ": ";
                if (!num_received.has_value()) ss << num_received.error();
                else if (*num_received != received) ss << "count mismatch";
                else ss << "handler fired but no sample available";
                ss << ", terminating.\n";
                std::cerr << ss.str();
                forward_event.Unsubscribe();
                return EXIT_FAILURE;
            }

            if (*num_received >= 1U)
            {
                std::cout << ToString(instance_specifier, ": Proxy received valid data\n");
                cycle += *num_received;
            }

            const auto elapsed = std::chrono::steady_clock::now() - cycle_start;
            std::cout << ToString(instance_specifier, ": Cycle duration ",
                                  std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
                                  "ms\n");
            event_received.reset();
        }

        std::cout << ToString(instance_specifier, ": Unsubscribing...\n");
        forward_event.Unsubscribe();
        std::cout << ToString(instance_specifier, ": and terminating, bye bye\n");
        return EXIT_SUCCESS;
    }
    else  // Typed IpcBridgeProxy path — receive forward, send back on reverse
    {
        auto& forward_event = proxy.map_api_lanes_stamped_;
        auto& rev_sk = *reverse_skeleton_opt;

        concurrency::Notification event_received;
        if (!cycle_time.has_value())
        {
            forward_event.SetReceiveHandler([&event_received, &instance_specifier]() {
                std::cout << ToString(instance_specifier, ": Callback called\n");
                event_received.notify();
            });
        }

        std::cout << ToString(instance_specifier, ": Subscribing to map_api_lanes_stamped\n");
        forward_event.Subscribe(SAMPLES_PER_CYCLE);

        SampleReceiver forward_receiver{instance_specifier, "proxy<-skeleton", check_sample_hash};
        std::size_t reverse_cycle = 0U;

        for (std::size_t cycle = 0U; cycle < num_cycles;)
        {
            const auto cycle_start = std::chrono::steady_clock::now();
            if (cycle_time.has_value()) { std::this_thread::sleep_for(*cycle_time); }

            const auto received_before = forward_receiver.GetReceivedSampleCount();

            Result<std::size_t> num_received = forward_event.GetNewSamples(
                [&](SamplePtr<MapApiLanesStamped> sample) noexcept {
                    if (try_writing_to_data_segment)
                    {
                        auto* p = const_cast<MapApiLanesStamped*>(sample.get());
                        p->x += 1;
                    }
                    forward_receiver.ReceiveSample(*sample);
                },
                SAMPLES_PER_CYCLE);

            const auto received = forward_receiver.GetReceivedSampleCount() - received_before;

            if (!num_received.has_value() ||
                *num_received != received ||
                (*num_received == 0 && !cycle_time.has_value()))
            {
                std::stringstream ss;
                ss << instance_specifier << ": Error in cycle " << cycle << ": ";
                if (!num_received.has_value()) ss << num_received.error();
                else if (*num_received != received) ss << "count mismatch";
                else ss << "handler fired but no sample available";
                ss << ", terminating.\n";
                std::cerr << ss.str();
                forward_event.Unsubscribe();
                rev_sk.StopOfferService();
                return EXIT_FAILURE;
            }

            if (*num_received >= 1U)
            {
                std::cout << ToString(instance_specifier, ": Proxy received valid data\n");
                cycle += *num_received;

                // Send feedback back to skeleton on reverse channel
                for (std::size_t i = 0U; i < *num_received; ++i)
                {
                    auto back_sample = PrepareSample(
                        rev_sk.map_api_lanes_stamped_, reverse_cycle++, "proxy->skeleton");
                    if (back_sample.has_value())
                    {
                        rev_sk.map_api_lanes_stamped_.Send(std::move(back_sample).value());
                    }
                }
            }

            const auto elapsed = std::chrono::steady_clock::now() - cycle_start;
            std::cout << ToString(instance_specifier, ": Cycle duration ",
                                  std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
                                  "ms\n");
            event_received.reset();
        }

        std::cout << ToString(instance_specifier, ": Unsubscribing...\n");
        forward_event.Unsubscribe();
        rev_sk.StopOfferService();
        std::cout << ToString(instance_specifier, ": and terminating, bye bye\n");
        return EXIT_SUCCESS;
    }
}

// Explicit instantiations
template int EventSenderReceiver::RunAsProxy<IpcBridgeProxy, impl::ProxyEvent<MapApiLanesStamped>>(
    const score::mw::com::InstanceSpecifier&,
    const score::mw::com::InstanceSpecifier&,
    const score::cpp::optional<std::chrono::milliseconds>,
    const std::size_t, bool, bool);

template int EventSenderReceiver::RunAsProxy<impl::GenericProxy, impl::GenericProxyEvent>(
    const score::mw::com::InstanceSpecifier&,
    const score::mw::com::InstanceSpecifier&,
    const score::cpp::optional<std::chrono::milliseconds>,
    const std::size_t, bool, bool);

}  // namespace score::mw::com
