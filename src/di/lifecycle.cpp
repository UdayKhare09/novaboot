#include "novaboot/di/lifecycle.h"

#include <algorithm>
#include <stdexcept>

namespace novaboot::di {

void LifecycleManager::register_bean(void*                      instance,
                                     std::function<void(void*)> on_post_construct,
                                     std::function<void(void*)> on_pre_destroy) {
    entries_.push_back(LifecycleEntry{
        .instance          = instance,
        .on_post_construct = std::move(on_post_construct),
        .on_pre_destroy    = std::move(on_pre_destroy),
    });
}

void LifecycleManager::invoke_post_constructs() {
    for (auto& entry : entries_) {
        if (entry.on_post_construct) {
            entry.on_post_construct(entry.instance);
        }
    }
}

void LifecycleManager::invoke_pre_destroys() {
    // Reverse construction order — same as Spring
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->on_pre_destroy) {
            try {
                it->on_pre_destroy(it->instance);
            } catch (...) {
                // Swallow exceptions in pre_destroy — still destroy remaining beans
            }
        }
    }
}

} // namespace novaboot::di
