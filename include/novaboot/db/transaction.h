#pragma once

#include "novaboot/db/db_client.h"

#include <memory>
#include <stdexcept>
#include <utility>

namespace novaboot::db {

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

} // namespace novaboot::db
