// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 OtterStax
#include "connector.hpp"
#include "config/connections/connection_config_reader.hpp"
#include "utility/tracy_profiler.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>

namespace snowflake {
    namespace {
        SQLPOINTER integer_attribute(uintptr_t value) { return reinterpret_cast<SQLPOINTER>(value); }

        void append_parameter(std::string& out, std::string_view key, std::string_view value) {
            out.append(key).append("={");
            for (char c : value) {
                out.push_back(c);
                if (c == '}') out.push_back('}');
            }
            out.append("};");
        }

        struct statement_t {
            OdbcApi& api;
            SQLHSTMT handle{SQL_NULL_HSTMT};
            ~statement_t() { if (handle) api.free(SQL_HANDLE_STMT, handle); }
        };

        bool binary_type(SQLSMALLINT t) {
            return t == SQL_BINARY || t == SQL_VARBINARY || t == SQL_LONGVARBINARY;
        }
    }

    Connector::Connector(std::pmr::memory_resource* resource, config::SnowflakeConnectionDesc config,
                         std::unique_ptr<OdbcApi> api)
        : resource_(resource), config_(std::move(config)), api_(std::move(api)) {}

    Connector::~Connector() { close(); }

    core::error_t Connector::invalid(std::string_view what) const {
        return core::error_t{core::error_code_t::invalid_parameter, std::pmr::string{what, resource_}};
    }

    core::error_t Connector::failure(SQLSMALLINT type, SQLHANDLE handle, std::string_view operation) {
        // Driver diagnostics can include the connection string or SQL literals.
        // Return diagnostic codes without copying credentials into logs/errors.
        std::string message = "Snowflake " + std::string(operation) + " failed";
        core::error_code_t code = core::error_code_t::io_error;
        for (SQLSMALLINT i = 1; i <= 16; ++i) {
            std::array<SQLCHAR, 6> state{};
            SQLINTEGER native{};
            SQLSMALLINT length{};
            auto rc = api_->diagnostic(type, handle, i, state.data(), &native, nullptr, 0, &length);
            if (!SQL_SUCCEEDED(rc)) break;
            const std::string_view s{reinterpret_cast<const char*>(state.data()), 5};
            if (i == 1) {
                if (s.starts_with("22") || s == "01004" || s == "01S07") code = core::error_code_t::conversion_failure;
                else if (s.starts_with("23")) code = core::error_code_t::invalid_constraint;
                else if (s.starts_with("40")) code = core::error_code_t::write_conflict;
                else if (s == "42601") code = core::error_code_t::sql_parse_error;
                else if (!s.starts_with("08") && !s.starts_with("HY") && !s.starts_with("IM")) code = core::error_code_t::other_error;
            }
            message.append(" [").append(s).append("/").append(std::to_string(native)).append("]");
        }
        return core::error_t{code, std::pmr::string{message, resource_}};
    }

    bool Connector::truncation_only(SQLHSTMT statement) {
        bool found = false;
        for (SQLSMALLINT i = 1; i <= 16; ++i) {
            std::array<SQLCHAR, 6> state{};
            SQLINTEGER native{};
            SQLSMALLINT length{};
            auto rc = api_->diagnostic(SQL_HANDLE_STMT, statement, i, state.data(), &native, nullptr, 0, &length);
            if (rc == SQL_NO_DATA) return found;
            if (!SQL_SUCCEEDED(rc) || std::string_view{reinterpret_cast<char*>(state.data()), 5} != "01004") return false;
            found = true;
        }
        return false;
    }

    core::error_t Connector::connect() {
        OTX_ZONE_N("snowflake::Connector::connect");
        if (connected_ || transaction_) return invalid("Snowflake connection is already open");
        close();
        if (auto error = config::validation_error(config_)) return invalid(*error);
        const char* token = std::getenv(config_.token_env.c_str());
        if (!token || !*token) return invalid("Snowflake token_env must name a populated environment variable");
        std::string settings;
        for (const auto& [key, value] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
                 {"DRIVER", config_.driver}, {"SERVER", config_.host}, {"ACCOUNT", config_.account},
                 {"UID", config_.username}, {"DATABASE", config_.database}, {"SCHEMA", config_.schema},
                 {"WAREHOUSE", config_.warehouse}, {"ROLE", config_.role},
                 {"AUTHENTICATOR", config_.authenticator}, {"TOKEN", token},
                 {"PROTOCOL", "https"}, {"VERIFY_HOSTNAME", "true"}, {"VERIFY_CERTIFICATES", "true"},
                 {"TLS_SKIP_VERIFY", "false"}, {"MIN_TLS_VERSION", "tls12"},
                 {"SERVER_SESSION_KEEP_ALIVE", "false"}, {"CLIENT_SESSION_KEEP_ALIVE", "false"},
                 {"RETRY_MAX_ATTEMPTS", "1"}}) append_parameter(settings, key, value);
        append_parameter(settings, "PORT", std::to_string(config_.port));
        if (!config_.ca_file.empty()) append_parameter(settings, "CUSTOM_ROOT_STORE_PATH", config_.ca_file);
        append_parameter(settings, "LOGIN_TIMEOUT", std::to_string(config_.connect_timeout_seconds));
        append_parameter(settings, "CONNECT_TIMEOUT", std::to_string(config_.connect_timeout_seconds));
        append_parameter(settings, "QUERY_TIMEOUT", std::to_string(config_.query_timeout_seconds));
        append_parameter(settings, "REQUEST_TIMEOUT", std::to_string(config_.query_timeout_seconds));
        append_parameter(settings, "LOGOUT_TOTAL_TIMEOUT_SECONDS", std::to_string(config_.connect_timeout_seconds));
        append_parameter(settings, "LOGOUT_REQUEST_TIMEOUT_SECONDS", std::to_string(config_.connect_timeout_seconds));
        append_parameter(settings, "LOGOUT_MAX_ATTEMPTS", "1");
        if (settings.size() > static_cast<size_t>(std::numeric_limits<SQLSMALLINT>::max()))
            return invalid("Snowflake connection settings exceed the ODBC length limit");
        const auto connect_error = [&](SQLSMALLINT type, SQLHANDLE handle, std::string_view operation) {
            auto error = failure(type, handle, operation);
            close();
            return error;
        };
        if (api_->alloc(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env_) != SQL_SUCCESS)
            return connect_error(SQL_HANDLE_ENV, env_, "allocate environment");
        if (api_->env_attr(env_, SQL_ATTR_ODBC_VERSION, integer_attribute(SQL_OV_ODBC3), 0) != SQL_SUCCESS)
            return connect_error(SQL_HANDLE_ENV, env_, "set ODBC version");
        if (api_->alloc(SQL_HANDLE_DBC, env_, &connection_) != SQL_SUCCESS)
            return connect_error(SQL_HANDLE_ENV, env_, "allocate connection");
        if (api_->connect_attr(connection_, SQL_ATTR_LOGIN_TIMEOUT, integer_attribute(config_.connect_timeout_seconds), 0) != SQL_SUCCESS)
            return connect_error(SQL_HANDLE_DBC, connection_, "set login timeout");
        auto rc = api_->connect(connection_, reinterpret_cast<SQLCHAR*>(settings.data()), static_cast<SQLSMALLINT>(settings.size()));
        connected_ = SQL_SUCCEEDED(rc);
        if (rc != SQL_SUCCESS) {
            auto error = failure(SQL_HANDLE_DBC, connection_, "connect");
            close();
            return error;
        }
        std::array<char, 32> version{};
        SQLSMALLINT length{};
        if (api_->info(connection_, SQL_DRIVER_VER, version.data(), version.size(), &length) != SQL_SUCCESS ||
            length < 0 || static_cast<size_t>(length) >= version.size() ||
            std::string_view(version.data(), static_cast<size_t>(length)) != "04.00.0000") {
            close();
            return invalid("Snowflake requires the verified ODBC 4.0.0 driver");
        }
        if (api_->connect_attr(connection_, SQL_ATTR_AUTOCOMMIT, integer_attribute(SQL_AUTOCOMMIT_ON), 0) != SQL_SUCCESS) {
            auto error = failure(SQL_HANDLE_DBC, connection_, "enable autocommit");
            close();
            return error;
        }
        return core::error_t::no_error();
    }

    core::result_wrapper_t<result_t> Connector::run(std::string_view sql, bool read_result) {
        if (!connected_) return invalid("Snowflake connection is closed; transaction operations are not reconnected");
        if (sql.empty() || sql.find('\0') != std::string_view::npos ||
            sql.size() > static_cast<size_t>(std::numeric_limits<SQLINTEGER>::max())) return invalid("Invalid Snowflake statement length");
        statement_t statement{*api_};
        if (api_->alloc(SQL_HANDLE_STMT, connection_, &statement.handle) != SQL_SUCCESS)
            return failure(SQL_HANDLE_DBC, connection_, "allocate statement");
        if (api_->stmt_attr(statement.handle, SQL_ATTR_QUERY_TIMEOUT, integer_attribute(config_.query_timeout_seconds), 0) != SQL_SUCCESS)
            return failure(SQL_HANDLE_STMT, statement.handle, "set query timeout");
        if (api_->stmt_attr(statement.handle, multi_statement_count_attribute, integer_attribute(1), 0) != SQL_SUCCESS)
            return failure(SQL_HANDLE_STMT, statement.handle, "require a single statement");
        const auto rc = api_->exec(statement.handle, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.data())), static_cast<SQLINTEGER>(sql.size()));
        if (rc != SQL_SUCCESS && rc != SQL_NO_DATA) return failure(SQL_HANDLE_STMT, statement.handle, "execute (write outcome may be uncertain)");
        if (!read_result) return result_t{};
        return read(statement.handle);
    }

    core::result_wrapper_t<result_t> Connector::execute(std::string_view sql) {
        OTX_ZONE_N("snowflake::Connector::execute");
        return run(sql, true);
    }

    core::error_t Connector::begin() {
        OTX_ZONE_N("snowflake::Connector::begin");
        if (transaction_) return invalid("Snowflake transaction is already active");
        auto result = run("BEGIN TRANSACTION", false);
        if (result.has_error()) {
            auto error = std::move(result.error());
            close();
            return error;
        }
        transaction_ = true;
        return core::error_t::no_error();
    }

    core::error_t Connector::finish(bool commit) {
        OTX_ZONE_N("snowflake::Connector::finish");
        if (!transaction_) return invalid("Snowflake transaction is not active");
        auto result = run(commit ? "COMMIT" : "ROLLBACK", false);
        transaction_ = false;
        close();
        if (result.has_error()) return std::move(result.error());
        return core::error_t::no_error();
    }

    void Connector::close() noexcept {
        OTX_ZONE_N("snowflake::Connector::close");
        if (connected_ && transaction_) {
            // Destruction must release the connection even if error allocation
            // fails during best-effort rollback. Disconnect also ends its session.
            try { (void)run("ROLLBACK", false); } catch (...) {}
        }
        transaction_ = false;
        if (connected_) api_->disconnect(connection_);
        connected_ = false;
        if (connection_) api_->free(SQL_HANDLE_DBC, connection_);
        connection_ = SQL_NULL_HDBC;
        if (env_) api_->free(SQL_HANDLE_ENV, env_);
        env_ = SQL_NULL_HENV;
    }

    core::result_wrapper_t<result_t> Connector::read(SQLHSTMT statement) {
        OTX_ZONE_N("snowflake::Connector::read");
        result_t result;
        uint64_t remaining = config_.max_result_bytes;
        const auto consume = [&](uint64_t n) { if (n > remaining) return false; remaining -= n; return true; };
        SQLSMALLINT count{};
        if (api_->column_count(statement, &count) != SQL_SUCCESS) return failure(SQL_HANDLE_STMT, statement, "read column count");
        if (count < 0 || !consume(static_cast<uint64_t>(count) * sizeof(column_t))) return invalid("Snowflake result metadata exceeds max_result_bytes");
        for (SQLUSMALLINT i = 1; i <= static_cast<SQLUSMALLINT>(count); ++i) {
            column_t column;
            std::vector<SQLCHAR> name(256);
            SQLSMALLINT length{};
            auto rc = api_->describe(statement, i, name.data(), static_cast<SQLSMALLINT>(name.size()), &length,
                                     &column.sql_type, &column.precision, &column.scale, &column.nullable);
            if (rc == SQL_SUCCESS_WITH_INFO && length >= static_cast<SQLSMALLINT>(name.size()) && truncation_only(statement)) {
                if (length == std::numeric_limits<SQLSMALLINT>::max()) return invalid("Snowflake column name exceeds the ODBC limit");
                name.resize(static_cast<size_t>(length) + 1);
                rc = api_->describe(statement, i, name.data(), static_cast<SQLSMALLINT>(name.size()), &length,
                                    &column.sql_type, &column.precision, &column.scale, &column.nullable);
            }
            if (rc != SQL_SUCCESS) return failure(SQL_HANDLE_STMT, statement, "describe column");
            if (length < 0 || static_cast<size_t>(length) >= name.size() || !consume(static_cast<uint64_t>(length)))
                return invalid("Snowflake result column name is truncated or exceeds max_result_bytes");
            column.name.assign(reinterpret_cast<char*>(name.data()), static_cast<size_t>(length));
            result.columns.push_back(std::move(column));
        }
        SQLLEN affected{};
        if (api_->row_count(statement, &affected) != SQL_SUCCESS) return failure(SQL_HANDLE_STMT, statement, "read affected row count");
        if (affected >= 0) result.affected_rows = affected;
        if (count == 0) return result;
        for (;;) {
            const auto fetched = api_->fetch(statement);
            if (fetched == SQL_NO_DATA) return result;
            if (fetched != SQL_SUCCESS) return failure(SQL_HANDLE_STMT, statement, "fetch row");
            if (result.rows.size() >= config_.max_rows ||
                !consume(sizeof(result.rows.front()) + static_cast<uint64_t>(count) * sizeof(std::optional<std::string>)))
                return invalid("Snowflake result exceeds max_rows or max_result_bytes");
            std::vector<std::optional<std::string>> row(static_cast<size_t>(count));
            for (SQLUSMALLINT i = 1; i <= static_cast<SQLUSMALLINT>(count); ++i) {
                const bool binary = binary_type(result.columns[i - 1].sql_type);
                std::string value;
                std::array<char, 8192> buffer{};
                const size_t capacity = buffer.size() - (binary ? 0 : 1);
                bool is_null = false;
                for (;;) {
                    SQLLEN length{};
                    auto rc = api_->data(statement, i, binary ? SQL_C_BINARY : SQL_C_CHAR, buffer.data(), buffer.size(), &length);
                    if (length == SQL_NULL_DATA && rc == SQL_SUCCESS && value.empty()) { is_null = true; break; }
                    if (rc != SQL_SUCCESS && (rc != SQL_SUCCESS_WITH_INFO || !truncation_only(statement)))
                        return failure(SQL_HANDLE_STMT, statement, "read cell");
                    size_t bytes{};
                    if (rc == SQL_SUCCESS_WITH_INFO) {
                        if (length != SQL_NO_TOTAL && (length < 0 || static_cast<uint64_t>(length) <= capacity))
                            return invalid("Snowflake driver returned inconsistent truncated-cell length");
                        bytes = capacity;
                    } else {
                        if (length < 0 || static_cast<uint64_t>(length) > capacity)
                            return invalid("Snowflake driver returned an ambiguous or truncated cell");
                        bytes = static_cast<size_t>(length);
                    }
                    if (bytes > config_.max_cell_bytes - value.size() || !consume(bytes))
                        return invalid("Snowflake result exceeds max_cell_bytes or max_result_bytes");
                    value.append(buffer.data(), bytes);
                    if (rc == SQL_SUCCESS) break;
                }
                if (!is_null) row[i - 1] = std::move(value);
            }
            result.rows.push_back(std::move(row));
        }
    }

    std::unique_ptr<IConnector> make_connector(std::pmr::memory_resource* resource, config::SnowflakeConnectionDesc config) {
        OTX_ZONE_N("snowflake::make_connector");
        return std::make_unique<Connector>(resource, std::move(config));
    }
}
