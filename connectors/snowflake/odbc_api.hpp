// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 OtterStax
#pragma once

#include <sql.h>
#include <sqlext.h>
#include <memory>

namespace snowflake {
    // SQL_SF_STMT_ATTR_MULTI_STATEMENT_COUNT from Snowflake's sf_odbc.h.
    inline constexpr SQLINTEGER multi_statement_count_attribute = 0x4108;
    // One API object belongs to one connector. The seam lets fault-injection
    // tests exercise the real handle and fetch lifecycle without an account.
    class OdbcApi {
    public:
        virtual ~OdbcApi() = default;
        virtual SQLRETURN alloc(SQLSMALLINT, SQLHANDLE, SQLHANDLE*) = 0;
        virtual SQLRETURN free(SQLSMALLINT, SQLHANDLE) = 0;
        virtual SQLRETURN env_attr(SQLHENV, SQLINTEGER, SQLPOINTER, SQLINTEGER) = 0;
        virtual SQLRETURN connect_attr(SQLHDBC, SQLINTEGER, SQLPOINTER, SQLINTEGER) = 0;
        virtual SQLRETURN connect(SQLHDBC, SQLCHAR*, SQLSMALLINT) = 0;
        virtual SQLRETURN disconnect(SQLHDBC) = 0;
        virtual SQLRETURN info(SQLHDBC, SQLUSMALLINT, SQLPOINTER, SQLSMALLINT, SQLSMALLINT*) = 0;
        virtual SQLRETURN stmt_attr(SQLHSTMT, SQLINTEGER, SQLPOINTER, SQLINTEGER) = 0;
        virtual SQLRETURN exec(SQLHSTMT, SQLCHAR*, SQLINTEGER) = 0;
        virtual SQLRETURN column_count(SQLHSTMT, SQLSMALLINT*) = 0;
        virtual SQLRETURN describe(SQLHSTMT, SQLUSMALLINT, SQLCHAR*, SQLSMALLINT, SQLSMALLINT*,
                                   SQLSMALLINT*, SQLULEN*, SQLSMALLINT*, SQLSMALLINT*) = 0;
        virtual SQLRETURN fetch(SQLHSTMT) = 0;
        virtual SQLRETURN data(SQLHSTMT, SQLUSMALLINT, SQLSMALLINT, SQLPOINTER, SQLLEN, SQLLEN*) = 0;
        virtual SQLRETURN row_count(SQLHSTMT, SQLLEN*) = 0;
        virtual SQLRETURN diagnostic(SQLSMALLINT, SQLHANDLE, SQLSMALLINT, SQLCHAR*, SQLINTEGER*,
                                     SQLCHAR*, SQLSMALLINT, SQLSMALLINT*) = 0;
    };

    std::unique_ptr<OdbcApi> make_odbc_api();
}
