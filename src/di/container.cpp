#include "novaboot/di/container.h"
#include "novaboot/router/router.h"

#include <algorithm>
#include <cassert>
#include <queue>
#include <spdlog/spdlog.h>

namespace novaboot::di {

// ─────────────────────────────────────────────────────────────────────────────
// ContainerBase::init_singleton
// ─────────────────────────────────────────────────────────────────────────────

void* ContainerBase::init_singleton(BeanRegistration& reg) {
    void* raw = reg.factory(*this);

    instances_[reg.type_id] = raw;

    if (!reg.qualifier.empty()) {
        // Key: "mangled_type_name:qualifier"
        auto key = reg.name + ":" + reg.qualifier;
        qualified_instances_[key] = raw;
    }

    lifecycle_.register_bean(raw, reg.post_construct_fn, reg.pre_destroy_fn);
    return raw;
}

// ─────────────────────────────────────────────────────────────────────────────
// RootContainer — cycle detection
// ─────────────────────────────────────────────────────────────────────────────

void RootContainer::detect_cycles() {
    enum class Color : std::uint8_t { White, Gray, Black };
    std::unordered_map<std::type_index, Color> color;
    std::vector<std::type_index> path;

    for (auto& [tid, _] : registrations_) color[tid] = Color::White;

    std::function<void(std::type_index)> dfs = [&](std::type_index tid) {
        color[tid] = Color::Gray;
        path.push_back(tid);
        auto it = registrations_.find(tid);
        if (it == registrations_.end()) {
            path.pop_back();
            return;
        }

        for (auto dep_tid : it->second.dep_type_ids) {
            auto dep_color_it = color.find(dep_tid);
            if (dep_color_it == color.end()) continue;  // unregistered dep, skip

            if (dep_color_it->second == Color::Gray) {
                // Cycle detected — build descriptive path
                std::string cycle_path;
                auto start_it = std::find(path.begin(), path.end(), dep_tid);
                if (start_it != path.end()) {
                    for (auto p_it = start_it; p_it != path.end(); ++p_it) {
                        auto reg_it = registrations_.find(*p_it);
                        if (reg_it != registrations_.end()) {
                            cycle_path += reg_it->second.name + " -> ";
                        }
                    }
                }
                auto dep_it = registrations_.find(dep_tid);
                std::string dep_name = (dep_it != registrations_.end())
                                       ? dep_it->second.name : dep_tid.name();
                cycle_path += dep_name;

                throw DIError(
                    "novaboot::di: Circular dependency detected: " + cycle_path);
            }

            if (dep_color_it->second == Color::White) {
                dfs(dep_tid);
            }
        }
        path.pop_back();
        color[tid] = Color::Black;
    };

    for (auto& [tid, reg] : registrations_) {
        if (color[tid] == Color::White) dfs(tid);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RootContainer — topological sort (Kahn's BFS)
// ─────────────────────────────────────────────────────────────────────────────

void RootContainer::build_topo_order() {
    std::unordered_map<std::type_index, std::size_t>              in_degree;
    std::unordered_map<std::type_index, std::vector<std::type_index>> dependents;

    for (auto& [tid, reg] : registrations_) {
        if (!in_degree.contains(tid)) in_degree[tid] = 0;
        for (auto dep_tid : reg.dep_type_ids) {
            if (registrations_.contains(dep_tid)) {
                in_degree[tid]++;
                dependents[dep_tid].push_back(tid);
            }
        }
    }

    std::queue<std::type_index> queue;
    for (auto& [tid, deg] : in_degree)
        if (deg == 0) queue.push(tid);

    topo_order_.clear();
    while (!queue.empty()) {
        auto tid = queue.front(); queue.pop();
        topo_order_.push_back(tid);
        for (auto dep : dependents[tid]) {
            if (--in_degree[dep] == 0) queue.push(dep);
        }
    }

    // If some nodes weren't added (due to cycle), still include them so build
    // at least reaches the cycle-detected error path
    if (topo_order_.size() < registrations_.size()) {
        for (auto& [tid, _] : registrations_) {
            bool found = false;
            for (auto& t : topo_order_) if (t == tid) { found = true; break; }
            if (!found) topo_order_.push_back(tid);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RootContainer — async instantiation
// ─────────────────────────────────────────────────────────────────────────────

void* RootContainer::instantiate(BeanRegistration& reg) {
    if (reg.is_async) {
        auto fut = reg.async_factory(*this);
        if (reg.async_timeout_ms == 0u) {
            return fut.get();
        }
        auto status = fut.wait_for(std::chrono::milliseconds(reg.async_timeout_ms));
        if (status == std::future_status::timeout) {
            throw DIError(
                "novaboot::di: Async bean '" + reg.name
                + "' timed out after " + std::to_string(reg.async_timeout_ms) + "ms");
        }
        return fut.get();
    }
    return reg.factory(*this);
}

// ─────────────────────────────────────────────────────────────────────────────
// RootContainer::build()
// ─────────────────────────────────────────────────────────────────────────────

void RootContainer::build() {
    if (built_) throw DIError("novaboot::di: RootContainer::build() called twice");

    spdlog::info("novaboot::di: Building container ({} registrations)",
                 registrations_.size());

    // 1. Cycle detection
    detect_cycles();

    // 2. Topological sort
    build_topo_order();

    // 3. Instantiate non-lazy singletons in dependency order
    for (auto tid : topo_order_) {
        auto it = registrations_.find(tid);
        if (it == registrations_.end()) continue;
        auto& reg = it->second;

        if (reg.scope == Scope::Prototype) continue;
        if (reg.is_lazy)                  continue;

        spdlog::debug("novaboot::di: Instantiating '{}'", reg.name);

        void* raw = instantiate(reg);

        instances_[tid] = raw;

        if (!reg.qualifier.empty()) {
            auto key = reg.name + ":" + reg.qualifier;
            qualified_instances_[key] = raw;
        }

        lifecycle_.register_bean(raw, reg.post_construct_fn, reg.pre_destroy_fn);
        owned_instances_.emplace_back(raw, reg.destructor_fn);
    }

    // 4. Fire post_construct on all singletons (in registration order)
    lifecycle_.invoke_post_constructs();

    built_ = true;
    spdlog::info("novaboot::di: Container built ({} singletons)", instances_.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// RootContainer::shutdown()
// ─────────────────────────────────────────────────────────────────────────────

void RootContainer::shutdown() {
    if (!built_) return;
    spdlog::info("novaboot::di: Shutting down container");

    lifecycle_.invoke_pre_destroys();

    // Destroy beans in reverse construction order
    for (auto it = owned_instances_.rbegin(); it != owned_instances_.rend(); ++it) {
        if (it->second && it->first) it->second(it->first);
    }
    owned_instances_.clear();
    instances_.clear();
    qualified_instances_.clear();
    built_ = false;
    spdlog::info("novaboot::di: Container shut down");
}

RootContainer::~RootContainer() {
    if (built_) {
        try { shutdown(); } catch (...) {}
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RootContainer — resolve lazy singletons on first access
// ─────────────────────────────────────────────────────────────────────────────

// Lazy resolution is handled in ContainerBase::resolve<T>() by checking
// registrations_ when the instance is not yet cached. We override it in a
// non-template helper for use from the template:
void* RootContainer::resolve_lazy_singleton(std::type_index tid) {
    std::lock_guard<std::recursive_mutex> lock(lazy_mutex_);

    // Double-check check if it was initialized while we were waiting for the lock
    auto inst_it = instances_.find(tid);
    if (inst_it != instances_.end()) {
        return inst_it->second;
    }

    auto it = registrations_.find(tid);
    if (it == registrations_.end()) return nullptr;
    auto& reg = it->second;
    if (!reg.is_lazy) return nullptr;

    void* raw = instantiate(reg);
    instances_[tid] = raw;

    if (!reg.qualifier.empty()) {
        auto key = reg.name + ":" + reg.qualifier;
        qualified_instances_[key] = raw;
    }

    if (reg.post_construct_fn) reg.post_construct_fn(raw);
    owned_instances_.emplace_back(raw, reg.destructor_fn);
    return raw;
}

// ─────────────────────────────────────────────────────────────────────────────
// RootContainer — child container factories
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<ShardContainer> RootContainer::make_shard_container() {
    if (!built_)
        throw DIError("novaboot::di: Cannot create ShardContainer before build()");
    return std::make_unique<ShardContainer>(*this);
}

// ─────────────────────────────────────────────────────────────────────────────
// ShardContainer
// ─────────────────────────────────────────────────────────────────────────────

void ShardContainer::initialize() {
    for (auto& [tid, reg] : registrations_) {
        if (reg.is_lazy) continue;

        void* raw = reg.factory(*this);
        instances_[tid] = raw;

        if (!reg.qualifier.empty()) {
            auto key = reg.name + ":" + reg.qualifier;
            qualified_instances_[key] = raw;
        }

        lifecycle_.register_bean(raw, reg.post_construct_fn, reg.pre_destroy_fn);
        owned_instances_.emplace_back(raw, reg.destructor_fn);
    }
    lifecycle_.invoke_post_constructs();
}

ShardContainer::~ShardContainer() {
    lifecycle_.invoke_pre_destroys();
    for (auto it = owned_instances_.rbegin(); it != owned_instances_.rend(); ++it)
        if (it->second && it->first) it->second(it->first);
}

std::unique_ptr<RequestContainer> ShardContainer::make_request_container() {
    return std::make_unique<RequestContainer>(*this);
}

std::unique_ptr<ConnectionContainer> ShardContainer::make_connection_container() {
    return std::make_unique<ConnectionContainer>(*this);
}

// ─────────────────────────────────────────────────────────────────────────────
// RequestContainer
// ─────────────────────────────────────────────────────────────────────────────

RequestContainer::~RequestContainer() {
    lifecycle_.invoke_pre_destroys();
    for (auto it = owned_instances_.rbegin(); it != owned_instances_.rend(); ++it)
        if (it->second && it->first) it->second(it->first);
}

// ─────────────────────────────────────────────────────────────────────────────
// ConnectionContainer
// ─────────────────────────────────────────────────────────────────────────────

ConnectionContainer::~ConnectionContainer() {
    lifecycle_.invoke_pre_destroys();
    for (auto it = owned_instances_.rbegin(); it != owned_instances_.rend(); ++it)
        if (it->second && it->first) it->second(it->first);
}

void RootContainer::register_routes_and_advice(router::Router& router) {
    for (auto& registrar : route_registrars_) {
        registrar(router);
    }
}

} // namespace novaboot::di
