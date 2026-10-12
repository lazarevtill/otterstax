// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 OtterStax
#pragma once

#include "odbc_api.hpp"
#include "config/connections/connection_config.hpp"
#include <core/result_wrapper.hpp>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace snowflake {
    struct column_t {
        std::string name;
        SQLSMALLINT sql_type{};
        SQLULEN precision{};
        SQLSMALLINT scale{};
        SQLSMALLINT nullable{};
    };

    struct result_t {
        std::vector<column_t> columns;
        // Exact text for numbers/dates, raw bytes for SQL binary types. NULL is
        // distinct from empty data. No intermediate floating-point conversion.
        std::vector<std::vector<std::optional<std::string>>> rows;
        std::optional<int64_t> affected_rows;
    };

    class IConnector {
    public:
        virtual ~IConnector() = default;
        virtual core::error_t connect() = 0;
        virtual core::result_wrapper_t<result_t> execute(std::string_view) = 0;
        virtual core::error_t begin() = 0;
        virtual core::error_t finish(bool commit) = 0;
        virtual void close() noexcept = 0;
    };

    // Synchronous ODBC operations belong to a serial integration actor, never a
    // frontend executor or a shared transaction/autocommit connection.
    class Connector final : public IConnector {
    public:
        Connector(std::pmr::memory_resource*, config::SnowflakeConnectionDesc,
                  std::unique_ptr<OdbcApi> = make_odbc_api());
        ~Connector() override;
        Connector(const Connector&) = delete;
        Connector& operator=(const Connector&) = delete;
        core::error_t connect() override;
        core::result_wrapper_t<result_t> execute(std::string_view) override;
        core::error_t begin() override;
        core::error_t finish(bool commit) override;
        void close() noexcept override;

    private:
        core::result_wrapper_t<result_t> run(std::string_view, bool read_result);
        core::error_t failure(SQLSMALLINT, SQLHANDLE, std::string_view);
        core::error_t invalid(std::string_view) const;
        bool truncation_only(SQLHSTMT);
        core::result_wrapper_t<result_t> read(SQLHSTMT);
        std::pmr::memory_resource* resource_;
        config::SnowflakeConnectionDesc config_;
        std::unique_ptr<OdbcApi> api_;
        SQLHENV env_{SQL_NULL_HENV};
        SQLHDBC connection_{SQL_NULL_HDBC};
        bool connected_{};
        bool transaction_{};
    };

    using connector_factory = std::unique_ptr<IConnector> (*)(std::pmr::memory_resource*, config::SnowflakeConnectionDesc);
    std::unique_ptr<IConnector> make_connector(std::pmr::memory_resource*, config::SnowflakeConnectionDesc);
}
