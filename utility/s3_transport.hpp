// SPDX-License-Identifier: Apache-2.0
// Copyright 2025-2026 OtterStax
#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace otterstax {
inline std::optional<std::string> validate_s3_transport(std::string_view scheme, std::string_view endpoint,
                                                       std::string_view ca_file) {
    if (scheme != "https" && scheme != "http") return "S3 scheme must be https or http";
    if (endpoint.find_first_of("/?#@ \t\r\n") != std::string_view::npos || endpoint.find('\0') != std::string_view::npos) {
        return "S3 endpoint must be a hostname with optional port, without a URL scheme, path or credentials";
    }
    if (ca_file.find('\0') != std::string_view::npos) return "S3 ca_file must not contain NUL";
    if (scheme == "http" && !ca_file.empty()) return "S3 ca_file requires https";
    return std::nullopt;
}
}
