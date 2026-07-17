#pragma once

#include "novaboot/db/db_client.h"

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace novaboot::db {

/// Explicit lazy relation holder for C++ entity models.
///
/// NovaBoot does not try to emulate Java bytecode proxies for direct fields.
/// Use `Lazy<T>` when a relation should load on first access:
///
///   [[= ManyToOne(FetchType::Lazy) ]]
///   [[= JoinColumn("author_id") ]]
///   novaboot::db::Lazy<Author> author;
///
/// `get()` performs the deferred load once and then returns the cached entity.
template<typename T>
class Lazy {
public:
    Lazy() = default;

    static Lazy loaded(T value, Parameter id) {
        Lazy lazy;
        lazy.value_ = std::move(value);
        lazy.id_ = std::move(id);
        return lazy;
    }

    static Lazy unloaded(Parameter id, std::function<T()> loader) {
        Lazy lazy;
        lazy.id_ = std::move(id);
        lazy.loader_ = std::move(loader);
        return lazy;
    }

    [[nodiscard]] bool loaded() const noexcept {
        return value_.has_value();
    }

    [[nodiscard]] bool has_identity() const noexcept {
        return id_.has_value();
    }

    [[nodiscard]] const std::optional<Parameter>& id_parameter() const noexcept {
        return id_;
    }

    T& get() {
        ensure_loaded();
        return *value_;
    }

    const T& get() const {
        ensure_loaded();
        return *value_;
    }

    T& operator*() { return get(); }
    const T& operator*() const { return get(); }

    T* operator->() { return &get(); }
    const T* operator->() const { return &get(); }

private:
    void ensure_loaded() const {
        if (value_) return;
        if (!loader_) {
            throw std::runtime_error("novaboot::db::Lazy relation has no loader");
        }
        value_ = loader_();
    }

    mutable std::optional<T> value_;
    std::optional<Parameter> id_;
    std::function<T()> loader_;
};

/// Explicit lazy collection holder for C++ entity models.
///
/// Use with collection associations:
///
///   [[= OneToMany("project", FetchType::Lazy) ]]
///   novaboot::db::LazyCollection<Article> articles;
///
/// Accessing the collection through `get()`, iteration, or mutation loads the
/// collection once and then reuses the cached vector. `count()` can use a
/// separate count loader when the ORM provides one, avoiding full hydration.
template<typename T>
class LazyCollection {
public:
    LazyCollection() : state_(std::make_shared<State>()) {}

    static LazyCollection loaded(std::vector<T> values) {
        LazyCollection collection;
        collection.state_->values = std::move(values);
        return collection;
    }

    static LazyCollection unloaded(std::function<std::vector<T>()> loader,
                                   std::function<std::int64_t()> count_loader = {}) {
        LazyCollection collection;
        collection.state_->loader = std::move(loader);
        collection.state_->count_loader = std::move(count_loader);
        return collection;
    }

    [[nodiscard]] bool loaded() const noexcept {
        return state_->values.has_value();
    }

    std::vector<T>& get() {
        ensure_loaded();
        return *state_->values;
    }

    const std::vector<T>& get() const {
        ensure_loaded();
        return *state_->values;
    }

    [[nodiscard]] std::int64_t count() const {
        if (state_->values) return static_cast<std::int64_t>(state_->values->size());
        if (state_->count_loader) return state_->count_loader();
        return static_cast<std::int64_t>(get().size());
    }

    [[nodiscard]] std::size_t size() const {
        return static_cast<std::size_t>(count());
    }

    [[nodiscard]] bool empty() const {
        return count() == 0;
    }

    void push_back(const T& value) {
        get().push_back(value);
    }

    void push_back(T&& value) {
        get().push_back(std::move(value));
    }

    template<typename... Args>
    T& emplace_back(Args&&... args) {
        return get().emplace_back(std::forward<Args>(args)...);
    }

    auto begin() { return get().begin(); }
    auto end() { return get().end(); }
    auto begin() const { return get().begin(); }
    auto end() const { return get().end(); }

    T& operator[](std::size_t index) { return get()[index]; }
    const T& operator[](std::size_t index) const { return get()[index]; }

private:
    struct State {
        mutable std::optional<std::vector<T>> values;
        std::function<std::vector<T>()> loader;
        std::function<std::int64_t()> count_loader;
    };

    void ensure_loaded() const {
        if (state_->values) return;
        if (!state_->loader) {
            state_->values = std::vector<T>{};
            return;
        }
        state_->values = state_->loader();
    }

    std::shared_ptr<State> state_;
};

} // namespace novaboot::db
