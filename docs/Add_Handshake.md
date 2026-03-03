# Main modifications to add handshake

## File 1 — sample_sender_receiver.h
Add one line in the private section, right after last_ack_cycle_:
```cpp
// BEFORE:
score::cpp::optional<std::uint32_t> last_ack_cycle{};
std::mutex ack_mutex_{};
std::condition_variable ack_received_cv_{};

// AFTER:
score::cpp::optional<std::uint32_t> last_ack_cycle{};
bool sync_acknowledged_{false};    // ← ADD THIS LINE
std::mutex ack_mutex_{};
std::condition_variable ack_received_cv_{};
```
Also add one include at the top, after <condition_variable>:
```cpp
#include <functional>    // ← ADD THIS — needed for std::function in SampleReceiver
```
That's all for the header file.
