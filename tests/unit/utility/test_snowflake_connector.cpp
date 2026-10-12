// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 OtterStax
#include "connectors/snowflake/connector.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>

namespace {
    class environment_t {
    public:
        environment_t() {
            if (const char* value = std::getenv(name)) previous_ = value;
            set("fixture-token;with}delimiters");
        }
        ~environment_t() { set(previous_.value_or("").c_str()); }
        void set(const char* value) {
#ifdef _WIN32
            _putenv_s(name, value);
#else
            if (*value) setenv(name, value, 1); else unsetenv(name);
#endif
        }
        static constexpr const char* name = "OTTERSTAX_ODBC_TEST_TOKEN";
    private:
        std::optional<std::string> previous_;
    };

    config::SnowflakeConnectionDesc connection_config() {
        config::SnowflakeConnectionDesc c;
        c.alias = "warehouse";
        c.host = "account.snowflakecomputing.com";
        c.account = "org-account";
        c.username = "SERVICE_USER";
        c.database = "DB";
        c.warehouse = "COMPUTE_WH";
        c.role = "ROLE";
        c.token_env = environment_t::name;
        return c;
    }

    class FakeOdbc final : public snowflake::OdbcApi {
    public:
        snowflake::result_t result;
        std::vector<std::string> statements;
        std::string connection_settings;
        std::string version = "04.00.0000";
        std::string diagnostic_state;
        std::string fail_statement;
        SQLRETURN connect_result{SQL_SUCCESS};
        SQLRETURN exec_result{SQL_SUCCESS};
        SQLRETURN statement_attribute_result{SQL_SUCCESS};
        SQLSMALLINT fail_alloc_type{};
        std::map<SQLINTEGER, uintptr_t> statement_attributes;
        bool unknown_length{};
        bool inconsistent_length{};
        int connections{}, disconnections{}, statement_handles{}, connection_handles{}, env_handles{};
        SQLHDBC established{};
        std::vector<SQLHDBC> statement_connections;

        SQLRETURN alloc(SQLSMALLINT type, SQLHANDLE parent, SQLHANDLE* handle) override {
            if (type == fail_alloc_type) return SQL_ERROR;
            *handle = reinterpret_cast<SQLHANDLE>(static_cast<uintptr_t>(type + 1));
            if (type == SQL_HANDLE_ENV) ++env_handles;
            if (type == SQL_HANDLE_DBC) ++connection_handles;
            if (type == SQL_HANDLE_STMT) { ++statement_handles; statement_connections.push_back(parent); }
            return SQL_SUCCESS;
        }
        SQLRETURN free(SQLSMALLINT type, SQLHANDLE) override {
            if (type == SQL_HANDLE_ENV) --env_handles;
            if (type == SQL_HANDLE_DBC) --connection_handles;
            if (type == SQL_HANDLE_STMT) --statement_handles;
            return SQL_SUCCESS;
        }
        SQLRETURN env_attr(SQLHENV, SQLINTEGER, SQLPOINTER, SQLINTEGER) override { return SQL_SUCCESS; }
        SQLRETURN connect_attr(SQLHDBC, SQLINTEGER, SQLPOINTER, SQLINTEGER) override { return SQL_SUCCESS; }
        SQLRETURN connect(SQLHDBC h, SQLCHAR* s, SQLSMALLINT n) override {
            ++connections;
            established = h;
            connection_settings.assign(reinterpret_cast<char*>(s), n);
            return connect_result;
        }
        SQLRETURN disconnect(SQLHDBC) override { ++disconnections; return SQL_SUCCESS; }
        SQLRETURN info(SQLHDBC, SQLUSMALLINT, SQLPOINTER out, SQLSMALLINT capacity, SQLSMALLINT* length) override {
            *length = static_cast<SQLSMALLINT>(version.size());
            std::memcpy(out, version.c_str(), std::min<size_t>(capacity, version.size() + 1));
            return SQL_SUCCESS;
        }
        SQLRETURN stmt_attr(SQLHSTMT, SQLINTEGER attribute, SQLPOINTER value, SQLINTEGER) override {
            statement_attributes[attribute] = reinterpret_cast<uintptr_t>(value);
            return statement_attribute_result;
        }
        SQLRETURN exec(SQLHSTMT, SQLCHAR* sql, SQLINTEGER n) override {
            statements.emplace_back(reinterpret_cast<char*>(sql), n);
            next_row_ = 0;
            positions_.assign(result.columns.size(), 0);
            if (statements.back() == fail_statement) return SQL_ERROR;
            return exec_result;
        }
        SQLRETURN column_count(SQLHSTMT, SQLSMALLINT* n) override { *n = static_cast<SQLSMALLINT>(result.columns.size()); return SQL_SUCCESS; }
        SQLRETURN describe(SQLHSTMT, SQLUSMALLINT i, SQLCHAR* name, SQLSMALLINT capacity, SQLSMALLINT* length,
                           SQLSMALLINT* type, SQLULEN* precision, SQLSMALLINT* scale, SQLSMALLINT* nullable) override {
            const auto& c = result.columns.at(i - 1);
            *length = static_cast<SQLSMALLINT>(c.name.size()); *type = c.sql_type;
            *precision = c.precision; *scale = c.scale; *nullable = c.nullable;
            size_t size = std::min<size_t>(capacity - 1, c.name.size());
            std::memcpy(name, c.name.data(), size); name[size] = 0;
            if (size < c.name.size()) { diagnostic_state = "01004"; return SQL_SUCCESS_WITH_INFO; }
            return SQL_SUCCESS;
        }
        SQLRETURN fetch(SQLHSTMT) override {
            if (next_row_ == result.rows.size()) return SQL_NO_DATA;
            ++next_row_;
            std::fill(positions_.begin(), positions_.end(), 0);
            return SQL_SUCCESS;
        }
        SQLRETURN data(SQLHSTMT, SQLUSMALLINT i, SQLSMALLINT type, SQLPOINTER out, SQLLEN capacity, SQLLEN* length) override {
            const auto& cell = result.rows.at(next_row_ - 1).at(i - 1);
            if (!cell) { *length = SQL_NULL_DATA; return SQL_SUCCESS; }
            auto& position = positions_.at(i - 1);
            const size_t available = cell->size() - position;
            const size_t payload = static_cast<size_t>(capacity) - (type == SQL_C_CHAR ? 1 : 0);
            const size_t copied = std::min(payload, available);
            std::memcpy(out, cell->data() + position, copied);
            if (type == SQL_C_CHAR) static_cast<char*>(out)[copied] = 0;
            position += copied;
            *length = static_cast<SQLLEN>(available);
            if (inconsistent_length) { *length = capacity + 1; return SQL_SUCCESS; }
            if (available > payload) {
                diagnostic_state = "01004";
                if (unknown_length) *length = SQL_NO_TOTAL;
                return SQL_SUCCESS_WITH_INFO;
            }
            return SQL_SUCCESS;
        }
        SQLRETURN row_count(SQLHSTMT, SQLLEN* n) override { *n = result.affected_rows.value_or(-1); return SQL_SUCCESS; }
        SQLRETURN diagnostic(SQLSMALLINT, SQLHANDLE, SQLSMALLINT i, SQLCHAR* state, SQLINTEGER* code,
                             SQLCHAR*, SQLSMALLINT, SQLSMALLINT* length) override {
            if (i != 1 || diagnostic_state.empty()) return SQL_NO_DATA;
            std::memcpy(state, diagnostic_state.c_str(), 6);
            *code = 123; *length = 0;
            return SQL_SUCCESS;
        }
    private:
        size_t next_row_{};
        std::vector<size_t> positions_;
    };

    struct fixture_t {
        environment_t environment;
        FakeOdbc* api;
        snowflake::Connector connector;
        explicit fixture_t(config::SnowflakeConnectionDesc c = connection_config())
            : api(new FakeOdbc), connector(std::pmr::new_delete_resource(), std::move(c), std::unique_ptr<FakeOdbc>(api)) {}
        void open() { REQUIRE_FALSE(connector.connect().contains_error()); }
    };
}

TEST_CASE("Snowflake connection pins transport and safely quotes credentials", "[snowflake][connector]") {
    auto c = connection_config();
    c.role = "role};TLS_SKIP_VERIFY=true";
    fixture_t f(c);
    f.open();
    CHECK(f.api->connection_settings.find("ROLE={role}};TLS_SKIP_VERIFY=true};") != std::string::npos);
    CHECK(f.api->connection_settings.find("TOKEN={fixture-token;with}}delimiters};") != std::string::npos);
    for (const auto* setting : {"PROTOCOL={https};", "VERIFY_HOSTNAME={true};", "VERIFY_CERTIFICATES={true};",
                                "TLS_SKIP_VERIFY={false};", "RETRY_MAX_ATTEMPTS={1};"})
        CHECK(f.api->connection_settings.find(setting) != std::string::npos);
    f.connector.close();
    CHECK(f.api->env_handles == 0);
    CHECK(f.api->connection_handles == 0);
}

TEST_CASE("Snowflake connection refuses missing credentials and incompatible drivers", "[snowflake][connector]") {
    fixture_t f;
    f.environment.set("");
    CHECK(f.connector.connect().contains_error());
    CHECK(f.api->connections == 0);
    f.environment.set("fixture-token");
    f.api->version = "03.09.0000";
    CHECK(f.connector.connect().contains_error());
    CHECK(f.api->disconnections == 1);
    CHECK(f.api->connection_handles == 0);
    CHECK(f.connector.execute("SELECT 1").has_error());
}

TEST_CASE("Snowflake transaction retains its connection and closes after commit", "[snowflake][connector]") {
    fixture_t f;
    f.open();
    REQUIRE_FALSE(f.connector.begin().contains_error());
    CHECK(f.connector.begin().contains_error());
    f.api->result.affected_rows = 7;
    auto write = f.connector.execute("INSERT INTO t SELECT 1");
    REQUIRE_FALSE(write.has_error());
    CHECK(write.value().affected_rows == 7);
    REQUIRE_FALSE(f.connector.finish(true).contains_error());
    CHECK(f.api->statements == std::vector<std::string>{"BEGIN TRANSACTION", "INSERT INTO t SELECT 1", "COMMIT"});
    CHECK(f.api->connections == 1);
    for (auto h : f.api->statement_connections) CHECK(h == f.api->established);
    CHECK(f.api->statement_handles == 0);
    CHECK(f.api->disconnections == 1);
    CHECK(f.api->statement_attributes.at(SQL_ATTR_QUERY_TIMEOUT) == 30);
    CHECK(f.api->statement_attributes.at(snowflake::multi_statement_count_attribute) == 1);
    CHECK(f.connector.execute("INSERT INTO t SELECT 2").has_error());
    CHECK(f.api->statements.size() == 3);
}

TEST_CASE("Snowflake lost commit is returned without reconnect or replay", "[snowflake][connector]") {
    fixture_t f;
    f.open();
    REQUIRE_FALSE(f.connector.begin().contains_error());
    f.api->fail_statement = "COMMIT";
    f.api->diagnostic_state = "08S01";
    auto error = f.connector.finish(true);
    CHECK(error.contains_error());
    CHECK(std::string(error.what).find("uncertain") != std::string::npos);
    CHECK(f.api->statements == std::vector<std::string>{"BEGIN TRANSACTION", "COMMIT"});
    CHECK(f.api->connections == 1);
    CHECK(f.api->disconnections == 1);
    CHECK(f.api->statement_handles == 0);
    CHECK(f.connector.finish(true).contains_error());
}

TEST_CASE("Snowflake close rolls back an unfinished transaction", "[snowflake][connector]") {
    fixture_t f;
    f.open();
    REQUIRE_FALSE(f.connector.begin().contains_error());
    f.connector.close();
    CHECK(f.api->statements == std::vector<std::string>{"BEGIN TRANSACTION", "ROLLBACK"});
    CHECK(f.api->disconnections == 1);
    CHECK(f.api->statement_handles == 0);
    CHECK(f.api->env_handles == 0);
}

TEST_CASE("Snowflake fetch preserves exact numeric text NULL empty and binary cells", "[snowflake][connector]") {
    fixture_t f;
    f.open();
    f.api->result.columns = {{"NUMBER", SQL_DECIMAL, 38, 8, SQL_NULLABLE},
                              {"EMPTY", SQL_VARCHAR, 10, 0, SQL_NULLABLE},
                              {"NULL", SQL_VARCHAR, 10, 0, SQL_NULLABLE},
                              {std::string(300, 'b'), SQL_VARBINARY, 20000, 0, SQL_NULLABLE}};
    std::string bytes(19000, '\0');
    bytes[8191] = static_cast<char>(0xff);
    bytes[8192] = 'A';
    f.api->result.rows = {{{"123456789012345678901234567890.12345678"}, {""}, std::nullopt, {bytes}}};
    auto result = f.connector.execute("SELECT * FROM t");
    REQUIRE_FALSE(result.has_error());
    CHECK(result.value().rows == f.api->result.rows);
    CHECK(result.value().columns[0].precision == 38);
    CHECK(result.value().columns[0].scale == 8);
    CHECK(result.value().columns[3].name.size() == 300);
    CHECK_FALSE(result.value().affected_rows.has_value());
    CHECK(f.api->statement_handles == 0);
}

TEST_CASE("Snowflake fetch assembles unknown-length truncated text without losing bytes", "[snowflake][connector]") {
    fixture_t f;
    f.open();
    f.api->result.columns = {{"text", SQL_VARCHAR, 30000, 0, SQL_NULLABLE}};
    f.api->result.rows = {{{std::string(20000, 'x')}}};
    f.api->unknown_length = true;
    auto result = f.connector.execute("SELECT text FROM t");
    REQUIRE_FALSE(result.has_error());
    CHECK(result.value().rows == f.api->result.rows);
}

TEST_CASE("Snowflake fetch enforces row cell and result bounds with handle cleanup", "[snowflake][connector]") {
    auto c = connection_config();
    SECTION("row limit") { c.max_rows = 1; }
    SECTION("cell limit") { c.max_cell_bytes = 3; }
    SECTION("result limit") { c.max_result_bytes = 120; c.max_cell_bytes = 120; }
    fixture_t f(c);
    f.open();
    f.api->result.columns = {{"text", SQL_VARCHAR, 10, 0, SQL_NULLABLE}};
    f.api->result.rows = {{{"abcd"}}, {{"efgh"}}};
    auto result = f.connector.execute("SELECT text FROM t");
    CHECK(result.has_error());
    CHECK(f.api->statement_handles == 0);
}

TEST_CASE("Snowflake fetch rejects a driver claiming an oversized successful cell", "[snowflake][connector]") {
    fixture_t f;
    f.open();
    f.api->result.columns = {{"text", SQL_VARCHAR, 10, 0, SQL_NULLABLE}};
    f.api->result.rows = {{{"abcd"}}};
    f.api->inconsistent_length = true;
    CHECK(f.connector.execute("SELECT text FROM t").has_error());
    CHECK(f.api->statement_handles == 0);
}

TEST_CASE("Snowflake execution errors retain diagnostic codes without credential text", "[snowflake][connector]") {
    fixture_t f;
    f.open();
    f.api->fail_statement = "bad statement";
    f.api->diagnostic_state = "42000";
    auto result = f.connector.execute("bad statement");
    REQUIRE(result.has_error());
    CHECK(std::string(result.error().what).find("42000/123") != std::string::npos);
    CHECK(std::string(result.error().what).find("fixture-token") == std::string::npos);
    CHECK(f.api->statement_handles == 0);
}

TEST_CASE("Snowflake real ODBC manager reports an unavailable driver without leaking credentials", "[snowflake][connector]") {
    environment_t environment;
    auto c = connection_config();
    c.driver = "otterstax-test-nonexistent-snowflake-driver";
    snowflake::Connector connector(std::pmr::new_delete_resource(), c);
    auto error = connector.connect();
    REQUIRE(error.contains_error());
    CHECK(std::string(error.what).find("fixture-token") == std::string::npos);
    CHECK(connector.execute("SELECT 1").has_error());
}

TEST_CASE("Snowflake partial connection allocation releases its environment", "[snowflake][connector]") {
    fixture_t f;
    f.api->fail_alloc_type = SQL_HANDLE_DBC;
    CHECK(f.connector.connect().contains_error());
    CHECK(f.api->env_handles == 0);
    CHECK(f.api->connection_handles == 0);
    CHECK(f.api->connections == 0);
}

TEST_CASE("Snowflake refuses execution when the driver changes a required statement attribute", "[snowflake][connector]") {
    fixture_t f;
    f.open();
    f.api->statement_attribute_result = SQL_SUCCESS_WITH_INFO;
    f.api->diagnostic_state = "01S02";
    CHECK(f.connector.execute("INSERT INTO t VALUES (1)").has_error());
    CHECK(f.api->statements.empty());
    CHECK(f.api->statement_handles == 0);
}

TEST_CASE("Snowflake failed begin closes the uncertain session", "[snowflake][connector]") {
    fixture_t f;
    f.open();
    f.api->fail_statement = "BEGIN TRANSACTION";
    f.api->diagnostic_state = "08S01";
    CHECK(f.connector.begin().contains_error());
    CHECK(f.connector.execute("INSERT INTO t VALUES (1)").has_error());
    CHECK(f.api->statements.size() == 1);
    CHECK(f.api->disconnections == 1);
    CHECK(f.api->statement_handles == 0);
    CHECK(f.api->connection_handles == 0);
}
