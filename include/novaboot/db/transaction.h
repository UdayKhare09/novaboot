#pragma once

#include "novaboot/annotations/stereotypes.h"
#include "novaboot/db/db_client.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#ifdef __cpp_impl_reflection
#include <meta>
#endif
#include <stdexcept>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

namespace novaboot::db {

namespace tx_detail {

#ifdef __cpp_impl_reflection
template<typename Ann>
consteval bool has_annotation(std::meta::info target) {
    for (auto ann : std::meta::annotations_of(std::meta::dealias(target))) {
        if (std::meta::is_same_type(std::meta::remove_cv(std::meta::type_of(ann)), ^^Ann)) {
            return true;
        }
    }
    return false;
}

template<typename Ann>
consteval Ann get_annotation(std::meta::info target) {
    for (auto ann : std::meta::annotations_of(std::meta::dealias(target))) {
        if (std::meta::is_same_type(std::meta::remove_cv(std::meta::type_of(ann)), ^^Ann)) {
            return std::meta::extract<Ann>(ann);
        }
    }
    return Ann{};
}

template<typename T>
consteval auto get_members() {
    constexpr auto ctx = std::meta::access_context::current();
    struct ArrayWrapper {
        std::meta::info data[128] = {};
        std::size_t size = 0;
        consteval const std::meta::info* begin() const noexcept { return data; }
        consteval const std::meta::info* end() const noexcept { return data + size; }
    };

    ArrayWrapper result;
    for (auto m : std::meta::members_of(std::meta::dealias(^^T), ctx)) {
        if (result.size < 128) result.data[result.size++] = m;
    }
    return result;
}

template<auto MethodPtr>
struct method_traits;

template<typename Class, typename Ret, typename... Args, Ret (Class::*MethodPtr)(Args...)>
struct method_traits<MethodPtr> {
    using class_type = Class;
    using return_type = Ret;
};

template<typename Class, typename Ret, typename... Args, Ret (Class::*MethodPtr)(Args...) const>
struct method_traits<MethodPtr> {
    using class_type = Class;
    using return_type = Ret;
};

template<std::meta::info Member, auto MethodPtr>
consteval bool is_matching_method() {
    if constexpr (std::meta::is_function(Member) &&
                  !std::meta::is_constructor(Member) &&
                  !std::meta::is_destructor(Member)) {
        if constexpr (std::meta::has_identifier(Member)) {
            constexpr std::string_view name = std::meta::identifier_of(Member);
            if constexpr (name.starts_with("operator")) {
                return false;
            } else if constexpr (std::is_same_v<decltype(&[:Member:]), decltype(MethodPtr)>) {
                return &[:Member:] == MethodPtr;
            }
        }
    }
    return false;
}

template<typename Class, auto MethodPtr>
consteval std::meta::info find_method_meta() {
    static constexpr auto members = get_members<Class>();
    template for (constexpr auto member : members) {
        if constexpr (is_matching_method<member, MethodPtr>()) {
            return member;
        }
    }
    return std::meta::info{};
}
#endif

} // namespace tx_detail

using TransactionPropagation = novaboot::annotations::TransactionPropagation;
using TransactionIsolation = novaboot::annotations::TransactionIsolation;

struct TransactionOptions {
    TransactionPropagation propagation = TransactionPropagation::Required;
    TransactionIsolation isolation = TransactionIsolation::Default;
    bool read_only = false;
    int timeout_seconds = 0;
    std::vector<std::type_index> rollback_for_types;
    std::vector<std::type_index> no_rollback_for_types;

    template<typename... Exceptions>
    TransactionOptions& rollback_for() {
        (rollback_for_types.emplace_back(typeid(Exceptions)), ...);
        return *this;
    }

    template<typename... Exceptions>
    TransactionOptions& no_rollback_for() {
        (no_rollback_for_types.emplace_back(typeid(Exceptions)), ...);
        return *this;
    }
};

class TransactionTimeoutException : public std::runtime_error {
public:
    explicit TransactionTimeoutException(int timeout_seconds)
        : std::runtime_error("Transaction timed out after " +
                             std::to_string(timeout_seconds) + " seconds") {}
};

class UnexpectedRollbackException : public std::runtime_error {
public:
    UnexpectedRollbackException()
        : std::runtime_error("Transaction was marked rollback-only") {}
};

/// An RAII transaction that pins one pooled connection for its entire scope.
/// Call commit() to make changes durable; otherwise the destructor rolls back.
class Transaction {
public:
    explicit Transaction(std::shared_ptr<DataSource> datasource)
        : connection_(datasource ? datasource->get_connection() : nullptr) {
        if (!connection_) {
            throw std::invalid_argument("Transaction requires a DataSource");
        }
        connection_->begin_transaction();
    }

    ~Transaction() {
        if (!completed_) {
            try {
                connection_->rollback();
            } catch (...) {
                // Destructors must not throw. The original exception, if any,
                // remains the one observed by the caller.
            }
        }
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    Transaction(Transaction&&) = delete;
    Transaction& operator=(Transaction&&) = delete;

    std::shared_ptr<Connection> connection() const { return connection_; }

    void commit() {
        require_active();
        connection_->commit();
        completed_ = true;
    }

    void rollback() {
        require_active();
        connection_->rollback();
        completed_ = true;
    }

private:
    void require_active() const {
        if (completed_) {
            throw std::logic_error("Transaction has already completed");
        }
    }

    std::shared_ptr<Connection> connection_;
    bool completed_ = false;
};

/// Service-layer transaction helper.
///
/// This is the practical C++ equivalent of Spring's transaction template:
/// the callback receives the active `Connection`, and any repositories created
/// with that connection participate in the same transaction. On normal return
/// the transaction commits; on exception it rolls back.
class TransactionManager {
public:
    explicit TransactionManager(std::shared_ptr<DataSource> datasource)
        : datasource_(std::move(datasource)) {
        if (!datasource_) {
            throw std::invalid_argument("TransactionManager requires a DataSource");
        }
    }

    [[nodiscard]] std::shared_ptr<DataSource> datasource() const noexcept {
        return datasource_;
    }

    Transaction begin() const {
        return Transaction(datasource_);
    }

    template<typename Fn>
    decltype(auto) execute(Fn&& fn) const {
        return execute(TransactionOptions{}, std::forward<Fn>(fn));
    }

    template<typename Fn>
    decltype(auto) execute(const TransactionOptions& options, Fn&& fn) const {
        auto* active = current_active();

        switch (options.propagation) {
            case TransactionPropagation::Required:
                if (active) {
                    return execute_existing(*active, options, std::forward<Fn>(fn));
                }
                return execute_new(options, std::forward<Fn>(fn));
            case TransactionPropagation::RequiresNew:
                return execute_new(options, std::forward<Fn>(fn));
            case TransactionPropagation::Supports:
                if (active) {
                    return execute_existing(*active, options, std::forward<Fn>(fn));
                }
                return execute_non_transactional(options, std::forward<Fn>(fn));
            case TransactionPropagation::NotSupported: {
                SuspendedTransactions suspended(datasource_.get());
                return execute_non_transactional(options, std::forward<Fn>(fn));
            }
            case TransactionPropagation::Mandatory:
                if (!active) {
                    throw std::logic_error("Transaction propagation MANDATORY requires an active transaction");
                }
                return execute_existing(*active, options, std::forward<Fn>(fn));
            case TransactionPropagation::Never:
                if (active) {
                    throw std::logic_error("Transaction propagation NEVER forbids an active transaction");
                }
                return execute_non_transactional(options, std::forward<Fn>(fn));
        }

        throw std::logic_error("Unknown transaction propagation");
    }

#ifdef __cpp_impl_reflection
    template<auto MethodPtr, typename Object, typename... Args>
    decltype(auto) invoke(Object& object, Args&&... args) const {
        using Traits = tx_detail::method_traits<MethodPtr>;
        using Class = typename Traits::class_type;
        constexpr auto method = tx_detail::find_method_meta<Class, MethodPtr>();
        static_assert(method != std::meta::info{}, "TransactionManager::invoke could not find method metadata");

        if constexpr (tx_detail::has_annotation<novaboot::annotations::Transactional>(method)) {
            auto options = options_from_annotation<method>();
            return execute(options, [&](std::shared_ptr<Connection>) -> decltype(auto) {
                return (object.*MethodPtr)(std::forward<Args>(args)...);
            });
        } else {
            return (object.*MethodPtr)(std::forward<Args>(args)...);
        }
    }
#endif

private:
    struct ActiveTransaction {
        const DataSource* datasource = nullptr;
        std::shared_ptr<Connection> connection;
        std::shared_ptr<bool> rollback_only;
    };

    inline static thread_local std::vector<ActiveTransaction> active_transactions_{};

public:
    static std::shared_ptr<Connection> current_connection_for(const DataSource* datasource) {
        for (auto it = active_transactions_.rbegin(); it != active_transactions_.rend(); ++it) {
            if (it->datasource == datasource) return it->connection;
        }
        return nullptr;
    }

private:

    class ActiveTransactionGuard {
    public:
        ActiveTransactionGuard(const DataSource* datasource,
                               std::shared_ptr<Connection> connection,
                               std::shared_ptr<bool> rollback_only) {
            active_transactions_.push_back(ActiveTransaction{
                .datasource = datasource,
                .connection = std::move(connection),
                .rollback_only = std::move(rollback_only),
            });
        }

        ~ActiveTransactionGuard() {
            active_transactions_.pop_back();
        }

        ActiveTransactionGuard(const ActiveTransactionGuard&) = delete;
        ActiveTransactionGuard& operator=(const ActiveTransactionGuard&) = delete;
    };

    class SuspendedTransactions {
    public:
        explicit SuspendedTransactions(const DataSource* datasource) {
            auto it = active_transactions_.begin();
            while (it != active_transactions_.end()) {
                if (it->datasource == datasource) {
                    suspended_.push_back(std::move(*it));
                    it = active_transactions_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        ~SuspendedTransactions() {
            active_transactions_.insert(active_transactions_.end(),
                                        std::make_move_iterator(suspended_.begin()),
                                        std::make_move_iterator(suspended_.end()));
        }

        SuspendedTransactions(const SuspendedTransactions&) = delete;
        SuspendedTransactions& operator=(const SuspendedTransactions&) = delete;

    private:
        std::vector<ActiveTransaction> suspended_;
    };

    ActiveTransaction* current_active() const {
        for (auto it = active_transactions_.rbegin(); it != active_transactions_.rend(); ++it) {
            if (it->datasource == datasource_.get()) return &*it;
        }
        return nullptr;
    }

    static bool matches(const std::vector<std::type_index>& types, const std::exception& ex) {
        const auto actual = std::type_index(typeid(ex));
        return std::ranges::find(types, actual) != types.end();
    }

    static bool should_rollback(const TransactionOptions& options, const std::exception& ex) {
        if (matches(options.no_rollback_for_types, ex)) return false;
        if (options.rollback_for_types.empty()) return true;
        return matches(options.rollback_for_types, ex);
    }

    void apply_transaction_options(const std::shared_ptr<Connection>& connection,
                                   const TransactionOptions& options) const {
        const auto dialect_name = datasource_->dialect()->name();
        if (dialect_name != "postgresql") {
            return;
        }

        if (options.isolation != TransactionIsolation::Default) {
            std::string level;
            switch (options.isolation) {
                case TransactionIsolation::ReadUncommitted:
                    level = "READ UNCOMMITTED";
                    break;
                case TransactionIsolation::ReadCommitted:
                    level = "READ COMMITTED";
                    break;
                case TransactionIsolation::RepeatableRead:
                    level = "REPEATABLE READ";
                    break;
                case TransactionIsolation::Serializable:
                    level = "SERIALIZABLE";
                    break;
                case TransactionIsolation::Default:
                    break;
            }
            if (!level.empty()) {
                connection->execute("SET TRANSACTION ISOLATION LEVEL " + level);
            }
        }

        if (options.read_only) {
            connection->execute("SET TRANSACTION READ ONLY");
        }
    }

    static void check_timeout(const std::chrono::steady_clock::time_point& started,
                              const TransactionOptions& options) {
        if (options.timeout_seconds <= 0) return;
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (elapsed > std::chrono::seconds(options.timeout_seconds)) {
            throw TransactionTimeoutException(options.timeout_seconds);
        }
    }

#ifdef __cpp_impl_reflection
    template<std::meta::info Method>
    static TransactionOptions options_from_annotation() {
        constexpr auto transactional =
            tx_detail::get_annotation<novaboot::annotations::Transactional>(Method);
        TransactionOptions options;
        options.propagation = transactional.propagation;
        options.isolation = transactional.isolation;
        options.read_only = transactional.read_only;
        options.timeout_seconds = transactional.timeout_seconds;
        return options;
    }
#endif

    template<typename Fn>
    decltype(auto) execute_non_transactional(const TransactionOptions& options, Fn&& fn) const {
        const auto started = std::chrono::steady_clock::now();
        auto connection = datasource_->get_connection();

        if constexpr (std::is_void_v<std::invoke_result_t<Fn, std::shared_ptr<Connection>>>) {
            std::invoke(std::forward<Fn>(fn), connection);
            check_timeout(started, options);
        } else {
            auto result = std::invoke(std::forward<Fn>(fn), connection);
            check_timeout(started, options);
            return result;
        }
    }

    template<typename Fn>
    decltype(auto) execute_existing(ActiveTransaction& active,
                                    const TransactionOptions& options,
                                    Fn&& fn) const {
        const auto started = std::chrono::steady_clock::now();

        try {
            if constexpr (std::is_void_v<std::invoke_result_t<Fn, std::shared_ptr<Connection>>>) {
                std::invoke(std::forward<Fn>(fn), active.connection);
                check_timeout(started, options);
            } else {
                auto result = std::invoke(std::forward<Fn>(fn), active.connection);
                check_timeout(started, options);
                return result;
            }
        } catch (const std::exception& ex) {
            if (should_rollback(options, ex)) {
                *active.rollback_only = true;
            }
            throw;
        } catch (...) {
            *active.rollback_only = true;
            throw;
        }
    }

    template<typename Fn>
    decltype(auto) execute_new(const TransactionOptions& options, Fn&& fn) const {
        const auto started = std::chrono::steady_clock::now();
        Transaction transaction(datasource_);
        apply_transaction_options(transaction.connection(), options);

        auto rollback_only = std::make_shared<bool>(false);
        ActiveTransactionGuard guard(datasource_.get(), transaction.connection(), rollback_only);

        try {
            if constexpr (std::is_void_v<std::invoke_result_t<Fn, std::shared_ptr<Connection>>>) {
                std::invoke(std::forward<Fn>(fn), transaction.connection());
                check_timeout(started, options);
                if (*rollback_only) {
                    transaction.rollback();
                    throw UnexpectedRollbackException();
                }
                transaction.commit();
            } else {
                auto result = std::invoke(std::forward<Fn>(fn), transaction.connection());
                check_timeout(started, options);
                if (*rollback_only) {
                    transaction.rollback();
                    throw UnexpectedRollbackException();
                }
                transaction.commit();
                return result;
            }
        } catch (const UnexpectedRollbackException&) {
            throw;
        } catch (const std::exception& ex) {
            if (should_rollback(options, ex)) {
                transaction.rollback();
            } else {
                transaction.commit();
            }
            throw;
        } catch (...) {
            transaction.rollback();
            throw;
        }
    }

    std::shared_ptr<DataSource> datasource_;
};

} // namespace novaboot::db
