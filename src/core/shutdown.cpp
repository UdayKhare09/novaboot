#include "novaboot/core/shutdown.h"

#include <thread>

namespace novaboot::core {

void ShutdownCoordinator::add(ShutdownParticipant participant) {
    participants_.push_back(std::move(participant));
}

ShutdownResult ShutdownCoordinator::drain_for(std::chrono::milliseconds timeout) const {
    for (const auto& participant : participants_) {
        if (participant.stop_accepting) participant.stop_accepting();
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        bool all_drained = true;
        for (const auto& participant : participants_) {
            if (participant.drained && !participant.drained()) {
                all_drained = false;
                break;
            }
        }
        if (all_drained) return {};
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ShutdownResult result{.drained = false, .forced_participants = {}};
    for (const auto& participant : participants_) {
        if (participant.drained && !participant.drained()) {
            result.forced_participants.push_back(participant.name);
            if (participant.force_close) participant.force_close();
        }
    }
    return result;
}

} // namespace novaboot::core
