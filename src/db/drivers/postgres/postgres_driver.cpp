#include "novaboot/db/drivers/postgres/postgres_driver.h"
#include <spdlog/spdlog.h>
#include "novaboot/db/exceptions.h"
#include <stdexcept>
#include <format>
#include <iostream>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <cerrno>
#include <poll.h>

namespace {
std::string time_to_string(std::chrono::system_clock::time_point tp) {
    auto time = std::chrono::system_clock::to_time_t(tp);
    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

std::string escape_bytea(const std::vector<std::uint8_t>& bytes) {
    std::stringstream ss;
    ss << "\\x";
    ss << std::hex << std::setfill('0');
    for (auto b : bytes) {
        ss << std::setw(2) << static_cast<int>(b);
    }
    return ss.str();
}

std::chrono::system_clock::time_point string_to_time(const std::string& s) {
    std::tm tm = {};
    std::stringstream ss(s);
    ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    return std::chrono::system_clock::from_time_t(timegm(&tm));
}

[[noreturn]] void throw_postgres_error(const std::string& sqlstate,
                                       const std::string& message,
                                       std::string_view prefix) {
    const auto full_message = std::string(prefix) + ": " + message;
    if (sqlstate == "23505") {
        throw novaboot::db::UniqueConstraintViolationException(full_message);
    }
    if (sqlstate == "23503") {
        throw novaboot::db::ForeignKeyConstraintViolationException(full_message);
    }
    if (sqlstate == "23502") {
        throw novaboot::db::NotNullConstraintViolationException(full_message);
    }
    if (sqlstate.starts_with("23")) {
        throw novaboot::db::ConstraintViolationException(full_message);
    }
    throw std::runtime_error(full_message);
}

void capture_postgres_error(PGresult* result, PGconn* connection,
                            std::string& sqlstate,
                            std::string& message) {
    if (result) {
        const char* state = PQresultErrorField(result, PG_DIAG_SQLSTATE);
        const char* result_message = PQresultErrorMessage(result);
        sqlstate = state ? state : "";
        message = result_message && *result_message
            ? result_message
            : PQerrorMessage(connection);
    } else {
        sqlstate.clear();
        message = PQerrorMessage(connection);
    }
}
}

namespace novaboot::db::postgres {

// ─── PostgresResultSet ────────────────────────────────────────────────────────

PostgresResultSet::PostgresResultSet(PGresult* res) : res_(res) {
    if (res_) {
        row_count_ = PQntuples(res_);
    }
}

PostgresResultSet::~PostgresResultSet() {
    if (res_) {
        PQclear(res_);
    }
}

bool PostgresResultSet::next() {
    if (current_row_ + 1 < row_count_) {
        current_row_++;
        return true;
    }
    return false;
}

bool PostgresResultSet::is_null(int col_index) {
    if (!res_ || current_row_ < 0 || current_row_ >= row_count_) return true;
    return PQgetisnull(res_, current_row_, col_index) != 0;
}

std::int64_t PostgresResultSet::get_int(int col_index) {
    if (is_null(col_index)) return 0;
    return std::stoll(PQgetvalue(res_, current_row_, col_index));
}

double PostgresResultSet::get_double(int col_index) {
    if (is_null(col_index)) return 0.0;
    return std::stod(PQgetvalue(res_, current_row_, col_index));
}

std::string PostgresResultSet::get_string(int col_index) {
    if (is_null(col_index)) return "";
    return PQgetvalue(res_, current_row_, col_index);
}

std::vector<std::uint8_t> PostgresResultSet::get_blob(int col_index) {
    if (is_null(col_index)) return {};
    std::string val = PQgetvalue(res_, current_row_, col_index);
    if (val.starts_with("\\x")) {
        std::vector<std::uint8_t> result;
        result.reserve((val.length() - 2) / 2);
        for (size_t i = 2; i < val.length(); i += 2) {
            std::string byteString = val.substr(i, 2);
            uint8_t byte = static_cast<uint8_t>(std::strtol(byteString.c_str(), nullptr, 16));
            result.push_back(byte);
        }
        return result;
    }
    return std::vector<std::uint8_t>(val.begin(), val.end());
}

bool PostgresResultSet::get_bool(int col_index) {
    if (is_null(col_index)) return false;
    std::string val = PQgetvalue(res_, current_row_, col_index);
    return val == "t" || val == "true" || val == "1";
}

Uuid PostgresResultSet::get_uuid(int col_index) {
    return Uuid::from_string(get_string(col_index));
}

std::chrono::system_clock::time_point PostgresResultSet::get_time(int col_index) {
    return string_to_time(get_string(col_index));
}

int PostgresResultSet::column_count() const {
    return res_ ? PQnfields(res_) : 0;
}

std::string_view PostgresResultSet::column_name(int col_index) const {
    if (!res_) return {};
    const char* name = PQfname(res_, col_index);
    return name ? std::string_view(name) : std::string_view();
}

// ─── PostgresConnection ──────────────────────────────────────────────────────

PostgresConnection::PostgresConnection(PGconn* conn, bool own_conn) 
    : conn_(conn), own_conn_(own_conn) {}

PostgresConnection::~PostgresConnection() {
    if (own_conn_ && conn_) {
        PQfinish(conn_);
    }
}

std::string PostgresConnection::convert_placeholders(std::string_view sql) {
    std::string out;
    out.reserve(sql.size() + 10);
    int count = 1;
    for (size_t i = 0; i < sql.size(); ++i) {
        if (sql[i] == '?') {
            out += "$" + std::to_string(count++);
        } else {
            out += sql[i];
        }
    }
    return out;
}

std::vector<std::string> PostgresConnection::serialize_params(const std::vector<Parameter>& params) {
    std::vector<std::string> serialized;
    serialized.reserve(params.size());
    for (const auto& param : params) {
        std::visit([&serialized](auto&& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>) {
                serialized.push_back("");
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                serialized.push_back(std::to_string(val));
            } else if constexpr (std::is_same_v<T, double>) {
                serialized.push_back(std::to_string(val));
            } else if constexpr (std::is_same_v<T, std::string>) {
                serialized.push_back(val);
            } else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
                // Escape bytea blob for PostgreSQL
                serialized.push_back(escape_bytea(val));
            } else if constexpr (std::is_same_v<T, bool>) {
                serialized.push_back(val ? "true" : "false");
            } else if constexpr (std::is_same_v<T, Uuid>) {
                serialized.push_back(val.to_string());
            } else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>) {
                serialized.push_back(time_to_string(val));
            }
        }, param);
    }
    return serialized;
}

std::int64_t PostgresConnection::execute(std::string_view sql, const std::vector<Parameter>& params) {
    log_query(sql, params);
    std::string converted_sql = convert_placeholders(sql);
    std::vector<std::string> str_params = serialize_params(params);
    std::vector<const char*> param_values;
    param_values.reserve(params.size());

    for (size_t i = 0; i < params.size(); ++i) {
        if (std::holds_alternative<std::nullptr_t>(params[i])) {
            param_values.push_back(nullptr);
        } else {
            param_values.push_back(str_params[i].c_str());
        }
    }

    PGresult* res = PQexecParams(
        conn_,
        converted_sql.c_str(),
        static_cast<int>(params.size()),
        nullptr, // Let Postgres infer types
        param_values.empty() ? nullptr : param_values.data(),
        nullptr, // Text format lengths are ignored
        nullptr, // All text formats
        0        // Text format result
    );

    if (!res) {
        throw std::runtime_error(std::format("Postgres execution failed: {}", PQerrorMessage(conn_)));
    }

    ExecStatusType status = PQresultStatus(res);
    std::int64_t affected = 0;
    if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) {
        char* tuples = PQcmdTuples(res);
        if (tuples && *tuples) {
            try {
                affected = std::stoll(tuples);
            } catch (...) {
                affected = 0;
            }
        }
    }

    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        std::string sqlstate;
        std::string message;
        capture_postgres_error(res, conn_, sqlstate, message);
        PQclear(res);
        throw_postgres_error(sqlstate, message, "Postgres execution failed");
    }
    PQclear(res);
    return affected;
}

std::vector<std::int64_t> PostgresConnection::execute_batch(
    std::string_view sql,
    const std::vector<std::vector<Parameter>>& parameter_sets) {
    std::vector<std::int64_t> affected_rows;
    affected_rows.reserve(parameter_sets.size());
    if (parameter_sets.empty()) return affected_rows;

    // Prepare once, then send all binds before waiting for results. libpq's
    // pipeline mode removes the per-row network round trip while retaining one
    // result and error boundary for each supplied parameter set.
    const std::string converted_sql = convert_placeholders(sql);
    // The unnamed statement is connection-scoped and replaced automatically by
    // the next unnamed prepare. That avoids leaking generated statement names
    // when an explicit transaction is left aborted for the caller to roll back.
    constexpr const char* statement_name = "";
    PGresult* prepared = PQprepare(conn_, statement_name,
                                   converted_sql.c_str(), 0, nullptr);
    if (prepared == nullptr) {
        throw std::runtime_error(std::format(
            "Postgres batch prepare failed: {}", PQerrorMessage(conn_)));
    }
    if (PQresultStatus(prepared) != PGRES_COMMAND_OK) {
        std::string sqlstate;
        std::string message;
        capture_postgres_error(prepared, conn_, sqlstate, message);
        PQclear(prepared);
        throw_postgres_error(sqlstate, message, "Postgres batch prepare failed");
    }
    PQclear(prepared);

    if (PQenterPipelineMode(conn_) == 0) {
        throw std::runtime_error(std::format(
            "Postgres batch pipeline setup failed: {}", PQerrorMessage(conn_)));
    }

    const bool restore_blocking_mode = PQisnonblocking(conn_) == 0;
    if (restore_blocking_mode && PQsetnonblocking(conn_, 1) != 0) {
        (void)PQexitPipelineMode(conn_);
        throw std::runtime_error(std::format(
            "Postgres batch could not enable non-blocking pipeline I/O: {}",
            PQerrorMessage(conn_)));
    }

    auto wait_for_socket = [&](short events) -> bool {
        pollfd descriptor{.fd = PQsocket(conn_), .events = events, .revents = 0};
        if (descriptor.fd < 0) return false;
        int result = 0;
        do {
            result = ::poll(&descriptor, 1, -1);
        } while (result < 0 && errno == EINTR);
        return result > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0;
    };

    bool send_failed = false;
    std::string send_error;
    for (const auto& parameters : parameter_sets) {
        log_query(sql, parameters);
        const auto serialized = serialize_params(parameters);
        std::vector<const char*> values;
        values.reserve(parameters.size());
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            values.push_back(std::holds_alternative<std::nullptr_t>(parameters[index])
                ? nullptr
                : serialized[index].c_str());
        }

        if (PQsendQueryPrepared(conn_, statement_name,
                                static_cast<int>(parameters.size()),
                                values.empty() ? nullptr : values.data(),
                                nullptr, nullptr, 0) == 0) {
            send_failed = true;
            send_error = PQerrorMessage(conn_);
            break;
        }
    }

    if (PQpipelineSync(conn_) == 0 && !send_failed) {
        send_failed = true;
        send_error = PQerrorMessage(conn_);
    }

    std::string sqlstate;
    std::string error_message;
    bool failed = send_failed;
    if (failed && error_message.empty()) error_message = std::move(send_error);

    bool saw_pipeline_sync = false;
    auto consume_result = [&](PGresult* result) {
        const auto status = PQresultStatus(result);
        if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) {
            char* tuples = PQcmdTuples(result);
            std::int64_t affected = 0;
            if (tuples != nullptr && *tuples != '\0') {
                try {
                    affected = std::stoll(tuples);
                } catch (...) {
                    affected = 0;
                }
            }
            affected_rows.push_back(affected);
        } else if (status == PGRES_PIPELINE_SYNC) {
            // Deliberate synchronization marker, not an application command.
            saw_pipeline_sync = true;
        } else if (!failed) {
            failed = true;
            capture_postgres_error(result, conn_, sqlstate, error_message);
            if (status == PGRES_PIPELINE_ABORTED && error_message.empty()) {
                error_message = "Postgres batch command was aborted by an earlier pipeline error";
            }
        }
        PQclear(result);
    };

    // In pipeline mode libpq is non-blocking. Flush outgoing commands, then
    // consume every response through the sync point before returning this
    // pooled connection to normal blocking operation.
    while (!failed) {
        const int flushed = PQflush(conn_);
        if (flushed == 0) break;
        if (flushed < 0 || !wait_for_socket(POLLOUT)) {
            failed = true;
            error_message = PQerrorMessage(conn_);
            break;
        }
    }

    bool fully_drained = false;
    while (!saw_pipeline_sync) {
        if (PGresult* result = PQgetResult(conn_)) {
            consume_result(result);
            continue;
        }

        // A null result separates result groups in pipeline mode. If libpq is
        // not busy, calling PQgetResult again starts the next group; only wait
        // when it reports that more socket input is required.
        if (PQisBusy(conn_) == 0) continue;
        if (!wait_for_socket(POLLIN) || PQconsumeInput(conn_) == 0) {
            failed = true;
            if (error_message.empty()) error_message = PQerrorMessage(conn_);
            break;
        }
    }
    fully_drained = saw_pipeline_sync;

    const bool exited_pipeline = fully_drained && PQexitPipelineMode(conn_) != 0;
    if (restore_blocking_mode) (void)PQsetnonblocking(conn_, 0);

    if (!exited_pipeline && !failed) {
        failed = true;
        error_message = PQerrorMessage(conn_);
    }
    if (failed) {
        if (error_message.empty()) error_message = "Postgres batch pipeline failed";
        throw_postgres_error(sqlstate, error_message, "Postgres batch execution failed");
    }
    if (affected_rows.size() != parameter_sets.size()) {
        throw std::runtime_error("Postgres batch execution ended without a result for every parameter set");
    }
    return affected_rows;
}

std::unique_ptr<ResultSet> PostgresConnection::query(std::string_view sql, const std::vector<Parameter>& params) {
    log_query(sql, params);
    std::string converted_sql = convert_placeholders(sql);
    std::vector<std::string> str_params = serialize_params(params);
    std::vector<const char*> param_values;
    param_values.reserve(params.size());

    for (size_t i = 0; i < params.size(); ++i) {
        if (std::holds_alternative<std::nullptr_t>(params[i])) {
            param_values.push_back(nullptr);
        } else {
            param_values.push_back(str_params[i].c_str());
        }
    }

    PGresult* res = PQexecParams(
        conn_,
        converted_sql.c_str(),
        static_cast<int>(params.size()),
        nullptr,
        param_values.empty() ? nullptr : param_values.data(),
        nullptr,
        nullptr,
        0
    );

    if (!res) {
        throw std::runtime_error(std::format("Postgres query failed: {}", PQerrorMessage(conn_)));
    }

    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
        std::string sqlstate;
        std::string message;
        capture_postgres_error(res, conn_, sqlstate, message);
        PQclear(res);
        throw_postgres_error(sqlstate, message, "Postgres query failed");
    }

    return std::make_unique<PostgresResultSet>(res);
}

std::int64_t PostgresConnection::last_insert_id() {
    auto res = query("SELECT lastval();");
    if (res && res->next()) {
        return res->get_int(0);
    }
    return 0;
}

void PostgresConnection::begin_transaction() {
    execute("BEGIN;");
}

void PostgresConnection::commit() {
    execute("COMMIT;");
}

void PostgresConnection::rollback() {
    execute("ROLLBACK;");
}

// ─── PostgresDataSource ──────────────────────────────────────────────────────

PostgresDataSource::PostgresDataSource(std::string conn_info, int pool_size,
                                       std::chrono::milliseconds acquisition_timeout,
                                       std::chrono::milliseconds leak_warning_threshold,
                                       std::string startup_sql)
    : conn_info_(std::move(conn_info)), pool_size_(pool_size),
      acquisition_timeout_(acquisition_timeout),
      leak_warning_threshold_(leak_warning_threshold), startup_sql_(std::move(startup_sql)) {
    if (pool_size_ <= 0 || acquisition_timeout_ <= std::chrono::milliseconds::zero() ||
        leak_warning_threshold_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("PostgreSQL pool size and timeouts must be positive");
    }
    for (int i = 0; i < pool_size_; ++i) {
        pool_->connections.push(create_connection());
    }
}

PostgresDataSource::~PostgresDataSource() {
    close();
}

PGconn* PostgresDataSource::create_connection() {
    PGconn* conn = PQconnectdb(conn_info_.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string err = PQerrorMessage(conn);
        PQfinish(conn);
        throw std::runtime_error(std::format("PostgreSQL connection failed: {}", err));
    }
    if (!startup_sql_.empty()) {
        PGresult* result = PQexec(conn, startup_sql_.c_str());
        const bool successful = result != nullptr && PQresultStatus(result) == PGRES_COMMAND_OK;
        const std::string error = successful ? "" : PQerrorMessage(conn);
        if (result != nullptr) PQclear(result);
        if (!successful) {
            PQfinish(conn);
            throw std::runtime_error(std::format(
                "PostgreSQL connection startup SQL failed: {}", error));
        }
    }
    return conn;
}

std::shared_ptr<Connection> PostgresDataSource::get_connection() {
    const auto pool = pool_;
    std::unique_lock<std::mutex> lock(pool->mutex);
    if (!pool->cv.wait_for(lock, acquisition_timeout_,
                           [&] { return !pool->connections.empty() || pool->closed; })) {
        throw std::runtime_error("PostgreSQL connection pool acquisition timed out");
    }

    if (pool->closed) {
        throw std::runtime_error("DataSource is closed");
    }

    PGconn* conn = pool->connections.front();
    pool->connections.pop();

    // Verify connection status
    if (PQstatus(conn) != CONNECTION_OK) {
        PQfinish(conn);
        try {
            conn = create_connection();
        } catch (...) {
            // Put remaining connections back and propagate
            pool->cv.notify_all();
            throw;
        }
    }

    const auto leased_at = std::chrono::steady_clock::now();
    const auto leak_warning_threshold = leak_warning_threshold_;
    auto cleanup = [pool, conn, leased_at, leak_warning_threshold](Connection* ptr) {
        delete ptr;
        const auto leased_for = std::chrono::steady_clock::now() - leased_at;
        if (leased_for >= leak_warning_threshold) {
            spdlog::warn("PostgreSQL connection lease returned after {}ms (threshold {}ms)",
                         std::chrono::duration_cast<std::chrono::milliseconds>(leased_for).count(),
                         leak_warning_threshold.count());
        }
        std::lock_guard<std::mutex> pool_lock(pool->mutex);
        if (!pool->closed) {
            pool->connections.push(conn);
            pool->cv.notify_one();
        } else {
            PQfinish(conn);
        }
    };

    return std::shared_ptr<Connection>(new PostgresConnection(conn, false), cleanup);
}

std::shared_ptr<SqlDialect> PostgresDataSource::dialect() {
    return dialect_;
}

void PostgresDataSource::close() {
    const auto pool = pool_;
    std::lock_guard<std::mutex> lock(pool->mutex);
    if (pool->closed) return;
    pool->closed = true;

    while (!pool->connections.empty()) {
        PGconn* conn = pool->connections.front();
        pool->connections.pop();
        PQfinish(conn);
    }
    pool->cv.notify_all();
}

} // namespace novaboot::db::postgres
