// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 OtterStax
#include "config/connections/connection_config_reader.hpp"
#include <catch2/catch_test_macros.hpp>

namespace {
    YAML::Node snowflake_config() {
        return YAML::Load(R"(
snowflake:
  - alias: warehouse
    host: org-account.snowflakecomputing.com
    account: org-account
    username: SERVICE_USER
    database: ANALYTICS
    schema: PUBLIC
    warehouse: COMPUTE_WH
    role: OTTERSTAX_ROLE
    token_env: OTTERSTAX_TEST_SNOWFLAKE_TOKEN
)");
    }
    auto parse(const YAML::Node& node) {
        return config::parse_connections(node, std::pmr::new_delete_resource());
    }
}

TEST_CASE("Snowflake config retains credential references and bounded defaults", "[config][snowflake]") {
    auto node = snowflake_config();
    auto result = parse(node);
    REQUIRE_FALSE(result.has_error());
    const auto& c = result.value().snowflake.at(0);
    CHECK(c.database == "ANALYTICS");
    CHECK(c.schema == "PUBLIC");
    CHECK(c.authenticator == "programmatic_access_token");
    CHECK(c.token_env == "OTTERSTAX_TEST_SNOWFLAKE_TOKEN");
    CHECK(c.connect_timeout_seconds == 10);
    CHECK(c.query_timeout_seconds == 30);
    CHECK(c.max_cell_bytes <= c.max_result_bytes);
    node["snowflake"][0]["authenticator"] = "oauth";
    node["snowflake"][0]["query_timeout_seconds"] = 45;
    auto oauth = parse(node);
    REQUIRE_FALSE(oauth.has_error());
    CHECK(oauth.value().snowflake[0].query_timeout_seconds == 45);
}

TEST_CASE("Snowflake config refuses malformed transport credentials and bounds", "[config][snowflake]") {
    for (const auto* key : {"alias", "host", "account", "username", "database", "schema", "warehouse", "role", "token_env"}) {
        auto node = snowflake_config();
        node["snowflake"][0][key] = "";
        INFO(key);
        CHECK(parse(node).has_error());
    }
    for (const auto* key : {"connect_timeout_seconds", "query_timeout_seconds", "max_rows", "max_result_bytes", "max_cell_bytes"}) {
        for (const auto* value : {"0", "-1", "1.5", "18446744073709551616"}) {
            auto node = snowflake_config();
            node["snowflake"][0][key] = value;
            INFO(key << "=" << value);
            CHECK(parse(node).has_error());
        }
    }
    auto node = snowflake_config();
    node["snowflake"][0]["host"] = "https://org-account.snowflakecomputing.com/path";
    CHECK(parse(node).has_error());
    node = snowflake_config();
    node["snowflake"][0]["token"] = "inline value is forbidden";
    CHECK(parse(node).has_error());
    node = snowflake_config();
    node["snowflake"][0]["token_env"] = "not-an-env-name";
    CHECK(parse(node).has_error());
    node = snowflake_config();
    node["snowflake"][0]["authenticator"] = "externalbrowser";
    CHECK(parse(node).has_error());
    node = snowflake_config();
    node["snowflake"][0]["max_result_bytes"] = 8;
    CHECK(parse(node).has_error());
    node = snowflake_config();
    node["snowflake"] = "invalid section";
    CHECK(parse(node).has_error());
}

TEST_CASE("Snowflake aliases share the remote backend namespace", "[config][snowflake]") {
    auto node = snowflake_config();
    node["mysql"] = YAML::Load("[{alias: warehouse, host: db, username: user, database: data}]");
    CHECK(parse(node).has_error());
}
