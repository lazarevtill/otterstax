// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 OtterStax
#include "odbc_api.hpp"
#include "utility/tracy_profiler.hpp"

namespace snowflake {
    namespace {
        class NativeOdbcApi final : public OdbcApi {
        public:
            SQLRETURN alloc(SQLSMALLINT t, SQLHANDLE p, SQLHANDLE* h) override { return SQLAllocHandle(t, p, h); }
            SQLRETURN free(SQLSMALLINT t, SQLHANDLE h) override { return SQLFreeHandle(t, h); }
            SQLRETURN env_attr(SQLHENV h, SQLINTEGER a, SQLPOINTER v, SQLINTEGER n) override {
                return SQLSetEnvAttr(h, a, v, n);
            }
            SQLRETURN connect_attr(SQLHDBC h, SQLINTEGER a, SQLPOINTER v, SQLINTEGER n) override {
                return SQLSetConnectAttr(h, a, v, n);
            }
            SQLRETURN connect(SQLHDBC h, SQLCHAR* s, SQLSMALLINT n) override {
                return SQLDriverConnect(h, nullptr, s, n, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
            }
            SQLRETURN disconnect(SQLHDBC h) override { return SQLDisconnect(h); }
            SQLRETURN info(SQLHDBC h, SQLUSMALLINT t, SQLPOINTER p, SQLSMALLINT n, SQLSMALLINT* l) override {
                return SQLGetInfo(h, t, p, n, l);
            }
            SQLRETURN stmt_attr(SQLHSTMT h, SQLINTEGER a, SQLPOINTER v, SQLINTEGER n) override {
                return SQLSetStmtAttr(h, a, v, n);
            }
            SQLRETURN exec(SQLHSTMT h, SQLCHAR* s, SQLINTEGER n) override { return SQLExecDirect(h, s, n); }
            SQLRETURN column_count(SQLHSTMT h, SQLSMALLINT* n) override { return SQLNumResultCols(h, n); }
            SQLRETURN describe(SQLHSTMT h, SQLUSMALLINT c, SQLCHAR* s, SQLSMALLINT n, SQLSMALLINT* l,
                               SQLSMALLINT* t, SQLULEN* p, SQLSMALLINT* scale, SQLSMALLINT* nullable) override {
                return SQLDescribeCol(h, c, s, n, l, t, p, scale, nullable);
            }
            SQLRETURN fetch(SQLHSTMT h) override { return SQLFetch(h); }
            SQLRETURN data(SQLHSTMT h, SQLUSMALLINT c, SQLSMALLINT t, SQLPOINTER p, SQLLEN n, SQLLEN* l) override {
                return SQLGetData(h, c, t, p, n, l);
            }
            SQLRETURN row_count(SQLHSTMT h, SQLLEN* n) override { return SQLRowCount(h, n); }
            SQLRETURN diagnostic(SQLSMALLINT t, SQLHANDLE h, SQLSMALLINT i, SQLCHAR* s, SQLINTEGER* code,
                                 SQLCHAR* text, SQLSMALLINT n, SQLSMALLINT* l) override {
                return SQLGetDiagRec(t, h, i, s, code, text, n, l);
            }
        };
    }

    std::unique_ptr<OdbcApi> make_odbc_api() {
        OTX_ZONE_N("snowflake::make_odbc_api");
        return std::make_unique<NativeOdbcApi>();
    }
}
