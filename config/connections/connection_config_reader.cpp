// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026  OtterStax

#include "connection_config_reader.hpp"

#include "utility/parse_port.hpp"
#include "utility/s3_transport.hpp"
#include "utility/tracy_profiler.hpp"
#include "yaml_scalar.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cctype>
#include <charconv>
#include <limits>
#include <map>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace config {
    namespace {
        // An invalid entry is reported as an invalid_parameter whose message names the
        // section and the entry, so the operator can find it in the file.
        core::error_t invalid_connection(const char* kind,
                                         const std::string& entry,
                                         const std::string& what,
                                         std::pmr::memory_resource* resource) {
            return core::error_t(
                core::error_code_t::invalid_parameter,
                std::pmr::string{(std::string("invalid ") + kind + " connection (" + entry + "): " + what).c_str(),
                                 resource});
        }

        // Reads the scalar fields of one connection entry. A missing key yields the
        // fallback; a key whose value is not a scalar is recorded as the entry's error
        // and every later read yields an empty string, so the first bad field is the
        // one reported.
        class entry_reader_t {
        public:
            entry_reader_t(const YAML::Node& node,
                           const char* kind,
                           std::size_t index,
                           std::pmr::memory_resource* resource)
                : node_(node)
                , kind_(kind)
                , index_(index)
                , resource_(resource) {}

            std::string str(const char* key, const std::string& fallback = "") {
                if (error_.contains_error() || !node_[key]) {
                    return fallback;
                }
                auto value = yaml_scalar<std::string>(node_[key], key, resource_);
                if (value.has_error()) {
                    error_ = invalid_connection(kind_,
                                                "entry #" + std::to_string(index_ + 1),
                                                std::string{value.error().what.data(), value.error().what.size()},
                                                resource_);
                    return "";
                }
                return std::move(value.value());
            }

            core::error_t take_error() { return std::move(error_); }

            std::string credential(const char* key) {
                const auto env_key = std::string{key} + "_env";
                if (!node_[env_key]) return str(key);
                const auto name = str(env_key.c_str());
                if (error_.contains_error()) return "";
                const bool valid_name = !name.empty() && (std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_') &&
                    std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
                const char* value = valid_name ? std::getenv(name.c_str()) : nullptr;
                if (node_[key] || !value || !*value) {
                    error_ = invalid_connection(kind_, "entry #" + std::to_string(index_ + 1),
                        env_key + ": requires a populated environment variable and no inline credential", resource_);
                    return "";
                }
                return value;
            }

        private:
            const YAML::Node& node_;
            const char* kind_;
            std::size_t index_;
            std::pmr::memory_resource* resource_;
            core::error_t error_{core::error_t::no_error()};
        };

        // Returns an error naming the first empty required field, or nullopt. `fields`
        // pairs each required field name with its value.
        std::optional<std::string>
        first_missing(std::initializer_list<std::pair<const char*, const std::string*>> fields) {
            for (const auto& [name, value] : fields) {
                if (value->empty()) {
                    return std::string("missing required field '") + name + "'";
                }
            }
            return std::nullopt;
        }

        // An empty port means "driver default"; anything else must satisfy the same
        // rule the connectors apply when they open the backend (decimal, 1..65535), so
        // a bad port is rejected here at startup instead of at the first open.
        std::optional<std::string> port_error(const std::string& port) {
            if (port.empty()) {
                return std::nullopt;
            }
            auto parsed = otterstax::parse_port(port, std::pmr::new_delete_resource());
            if (parsed.has_error()) {
                return std::string{parsed.error().what.data(), parsed.error().what.size()};
            }
            return std::nullopt;
        }

        // Required fields first, then the port: the first failing check names the
        // error, so an incomplete entry is reported as such even if its port is bad.
        std::optional<std::string>
        backend_validation_error(std::initializer_list<std::pair<const char*, const std::string*>> required,
                                 const std::string& port) {
            if (auto missing = first_missing(required)) {
                return missing;
            }
            return port_error(port);
        }

        // Fail fast: an incomplete connection must abort startup, not be silently
        // skipped — a half-configured backend is a config bug the operator has to fix.
        template<typename Desc>
        core::error_t validate(const Desc& d, const char* kind, std::pmr::memory_resource* resource) {
            if (auto err = validation_error(d)) {
                return invalid_connection(kind, "alias='" + d.alias + "'", *err, resource);
            }
            return core::error_t::no_error();
        }

        template<typename Desc>
        core::error_t
        validate_all(const std::vector<Desc>& descs, const char* kind, std::pmr::memory_resource* resource) {
            for (const auto& c : descs) {
                if (auto err = validate(c, kind, resource); err.contains_error()) {
                    return err;
                }
            }
            return core::error_t::no_error();
        }

        bool is_unquoted_identifier(const std::string& alias) {
            const auto lower_or_underscore = [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; };
            const auto digit = [](char c) { return c >= '0' && c <= '9'; };
            return !alias.empty() && lower_or_underscore(alias.front()) &&
                   std::all_of(alias.begin(), alias.end(), [&](char c) { return lower_or_underscore(c) || digit(c); });
        }

        core::error_t validate_backend_aliases(const ConnectionsConfig& config, std::pmr::memory_resource* resource) {
            std::map<std::string_view, const char*> seen;
            const auto check = [&](const auto& descs, const char* kind) {
                for (const auto& desc : descs) {
                    const std::string entry = "alias='" + desc.alias + "'";
                    if (!is_unquoted_identifier(desc.alias)) {
                        return invalid_connection(
                            kind,
                            entry,
                            "an alias is written unquoted in SQL: lower-case letters, digits and '_', "
                            "not starting with a digit",
                            resource);
                    }
                    if (auto [first, inserted] = seen.emplace(desc.alias, kind); !inserted) {
                        return invalid_connection(kind,
                                                  entry,
                                                  std::string("the alias already names a ") + first->second +
                                                      " connection",
                                                  resource);
                    }
                }
                return core::error_t::no_error();
            };
            if (auto err = check(config.mysql, "mysql"); err.contains_error()) {
                return err;
            }
            if (auto err = check(config.postgresql, "postgresql"); err.contains_error()) {
                return err;
            }
            if (auto err = check(config.clickhouse, "clickhouse"); err.contains_error()) return err;
            return check(config.snowflake, "snowflake");
        }
    } // namespace

    core::result_wrapper_t<ConnectionsConfig> parse_connections(const YAML::Node& connections_node,
                                                                std::pmr::memory_resource* resource) {
        OTX_ZONE_N("config::parse_connections");
        ConnectionsConfig result;

        if (!connections_node || !connections_node.IsMap()) {
            return result;
        }

        if (connections_node["mysql"] && connections_node["mysql"].IsSequence()) {
            std::size_t index = 0;
            for (const auto& node : connections_node["mysql"]) {
                entry_reader_t entry{node, "mysql", index++, resource};
                MysqlConnectionDesc desc{
                    .alias = entry.str("alias"),
                    .host = entry.str("host"),
                    .port = entry.str("port"),
                    .username = entry.str("username"),
                    .password = entry.str("password"),
                    .database = entry.str("database"),
                    .table = entry.str("table"),
                };
                if (auto err = entry.take_error(); err.contains_error()) {
                    return err;
                }
                result.mysql.push_back(std::move(desc));
            }
        }

        if (connections_node["postgresql"] && connections_node["postgresql"].IsSequence()) {
            std::size_t index = 0;
            for (const auto& node : connections_node["postgresql"]) {
                entry_reader_t entry{node, "postgresql", index++, resource};
                PgConnectionDesc desc{
                    .alias = entry.str("alias"),
                    .host = entry.str("host"),
                    .port = entry.str("port"),
                    .username = entry.str("username"),
                    .password = entry.str("password"),
                    .database = entry.str("database"),
                    .schema = entry.str("schema", "public"),
                    .table = entry.str("table"),
                };
                if (auto err = entry.take_error(); err.contains_error()) {
                    return err;
                }
                result.postgresql.push_back(std::move(desc));
            }
        }

        if (connections_node["clickhouse"] && connections_node["clickhouse"].IsSequence()) {
            std::size_t index = 0;
            for (const auto& node : connections_node["clickhouse"]) {
                entry_reader_t entry{node, "clickhouse", index++, resource};
                ChConnectionDesc desc{
                    .alias = entry.str("alias"),
                    .host = entry.str("host"),
                    .port = entry.str("port"),
                    .username = entry.str("username"),
                    .password = entry.str("password"),
                    .database = entry.str("database"),
                    .table = entry.str("table"),
                };
                if (auto err = entry.take_error(); err.contains_error()) {
                    return err;
                }
                result.clickhouse.push_back(std::move(desc));
            }
        }

        if (connections_node["snowflake"]) {
            if (!connections_node["snowflake"].IsSequence())
                return invalid_connection("snowflake", "section", "must be a sequence", resource);
            std::size_t index = 0;
            for (const auto& node : connections_node["snowflake"]) {
                if (!node.IsMap()) return invalid_connection("snowflake", "entry", "must be a mapping", resource);
                if (node["token"] || node["password"] || node["connection_string"])
                    return invalid_connection("snowflake", "entry", "use token_env; inline credentials and connection strings are not accepted", resource);
                entry_reader_t entry{node, "snowflake", index++, resource};
                SnowflakeConnectionDesc desc;
                desc.alias = entry.str("alias");
                desc.driver = entry.str("driver", desc.driver);
                desc.host = entry.str("host");
                desc.account = entry.str("account");
                desc.username = entry.str("username");
                desc.database = entry.str("database");
                desc.schema = entry.str("schema", desc.schema);
                desc.warehouse = entry.str("warehouse");
                desc.role = entry.str("role");
                desc.table = entry.str("table");
                desc.authenticator = entry.str("authenticator", desc.authenticator);
                desc.token_env = entry.str("token_env");
                auto number = [&](const char* key, auto& target) -> bool {
                    const auto text = entry.str(key, std::to_string(target));
                    uint64_t parsed{};
                    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
                    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed == 0 ||
                        parsed > std::numeric_limits<std::decay_t<decltype(target)>>::max()) return false;
                    target = static_cast<std::decay_t<decltype(target)>>(parsed);
                    return true;
                };
                if (!number("connect_timeout_seconds", desc.connect_timeout_seconds) ||
                    !number("query_timeout_seconds", desc.query_timeout_seconds) ||
                    !number("max_rows", desc.max_rows) || !number("max_result_bytes", desc.max_result_bytes) ||
                    !number("max_cell_bytes", desc.max_cell_bytes))
                    return invalid_connection("snowflake", desc.alias, "limits must be positive integers within their field range", resource);
                if (auto err = entry.take_error(); err.contains_error()) return err;
                result.snowflake.push_back(std::move(desc));
            }
        }

        if (connections_node["s3"] && connections_node["s3"].IsSequence()) {
            std::size_t index = 0;
            for (const auto& node : connections_node["s3"]) {
                entry_reader_t entry{node, "s3", index++, resource};
                S3CredentialDesc desc{
                    .alias = entry.str("alias"),
                    .access_key = entry.credential("access_key"),
                    .secret_key = entry.credential("secret_key"),
                    .region = entry.str("region"),
                    .session_token = entry.credential("session_token"),
                    .endpoint = entry.str("endpoint"),
                    .scheme = entry.str("scheme", "https"),
                    .ca_file = entry.str("ca_file"),
                };
                if (auto err = entry.take_error(); err.contains_error()) {
                    return err;
                }
                result.s3.push_back(std::move(desc));
            }
        }

        // Validate every parsed entry up front: the first incomplete connection is
        // the result, so the caller (ConfigReader::load → main) aborts startup
        // rather than coming up with a broken backend.
        if (auto err = validate_all(result.mysql, "mysql", resource); err.contains_error()) {
            return err;
        }
        if (auto err = validate_all(result.postgresql, "postgresql", resource); err.contains_error()) {
            return err;
        }
        if (auto err = validate_all(result.clickhouse, "clickhouse", resource); err.contains_error()) {
            return err;
        }
        if (auto err = validate_all(result.snowflake, "snowflake", resource); err.contains_error()) return err;
        if (auto err = validate_all(result.s3, "s3", resource); err.contains_error()) {
            return err;
        }
        if (auto err = validate_backend_aliases(result, resource); err.contains_error()) {
            return err;
        }

        return result;
    }

    // port is intentionally not required: an empty port makes the connector fall
    // back to the driver's default (see the addConnection implementations). A
    // non-empty port must be a valid TCP port.
    std::optional<std::string> validation_error(const MysqlConnectionDesc& c) {
        return backend_validation_error(
            {{"alias", &c.alias}, {"host", &c.host}, {"username", &c.username}, {"database", &c.database}},
            c.port);
    }

    std::optional<std::string> validation_error(const PgConnectionDesc& c) {
        return backend_validation_error(
            {{"alias", &c.alias}, {"host", &c.host}, {"username", &c.username}, {"database", &c.database}},
            c.port);
    }

    std::optional<std::string> validation_error(const ChConnectionDesc& c) {
        return backend_validation_error(
            {{"alias", &c.alias}, {"host", &c.host}, {"username", &c.username}, {"database", &c.database}},
            c.port);
    }

    std::optional<std::string> validation_error(const SnowflakeConnectionDesc& c) {
        OTX_ZONE_N("config::validation_error_snowflake");
        if (auto missing = first_missing({{"alias", &c.alias}, {"driver", &c.driver}, {"host", &c.host},
            {"account", &c.account}, {"username", &c.username}, {"database", &c.database}, {"schema", &c.schema},
            {"warehouse", &c.warehouse}, {"role", &c.role}, {"token_env", &c.token_env}})) return missing;
        if (c.authenticator != "oauth" && c.authenticator != "programmatic_access_token")
            return "authenticator must be oauth or programmatic_access_token";
        if (!(std::isalpha(static_cast<unsigned char>(c.token_env[0])) || c.token_env[0] == '_') ||
            !std::all_of(c.token_env.begin(), c.token_env.end(), [](unsigned char ch) { return std::isalnum(ch) || ch == '_'; }))
            return "token_env must name an environment variable";
        if (!std::all_of(c.host.begin(), c.host.end(), [](unsigned char ch) { return std::isalnum(ch) || ch == '.' || ch == '-'; }))
            return "host must be a DNS hostname without a scheme, port or path";
        for (const auto* value : {&c.driver, &c.account, &c.username, &c.database, &c.schema, &c.warehouse, &c.role, &c.table})
            if (std::any_of(value->begin(), value->end(), [](unsigned char ch) { return ch < 32 || ch == 127; }))
                return "connection fields must not contain control characters";
        if (c.connect_timeout_seconds == 0 || c.query_timeout_seconds == 0 || c.max_rows == 0 ||
            c.max_result_bytes == 0 || c.max_cell_bytes == 0 || c.max_cell_bytes > c.max_result_bytes)
            return "limits must be positive and max_cell_bytes must not exceed max_result_bytes";
        return std::nullopt;
    }

    std::optional<std::string> validation_error(const S3CredentialDesc& c) {
        if (auto missing = first_missing({{"alias", &c.alias}, {"access_key", &c.access_key}, {"secret_key", &c.secret_key}})) return missing;
        return otterstax::validate_s3_transport(c.scheme, c.endpoint, c.ca_file);
    }
} // namespace config
