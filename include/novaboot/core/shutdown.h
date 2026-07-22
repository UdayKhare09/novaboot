#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace novaboot::core {

/// A subsystem's shutdown contract. Registration is performed during startup;
/// stop_accepting, drained, and force_close execute during server shutdown.
struct ShutdownParticipant {
    std::string name;
    std::function<void()> stop_accepting;
    std::function<bool()> drained;
    std::function<void()> force_close;
};

struct ShutdownResult {
    bool drained = true;
    std::vector<std::string> forced_participants;
};

class ShutdownCoordinator {
public:
    void add(ShutdownParticipant participant);
    [[nodiscard]] ShutdownResult drain_for(std::chrono::milliseconds timeout) const;

private:
    std::vector<ShutdownParticipant> participants_;
};

} // namespace novaboot::core
