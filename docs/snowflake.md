# Snowflake datasource status

Snowflake datasource execution through the server is not available yet. This
feature branch contains driver dependencies, configuration validation and a raw
ODBC connector; it does not register a queryable backend. Reads, writes and
retained transactions remain the required feature scope. No live Snowflake
account is available for acceptance testing.

## Driver dependencies

Linux builds bundle Snowflake ODBC 4.0.0 and unixODBC 2.3.14 through the recipes
under `cmake/conan-recipes/`. Export those recipes before installing the pinned
Conan graph. Docker builds export them automatically. The driver archive hashes
are pinned for Linux x86_64 and arm64. Other operating systems require an
operator-installed Snowflake driver; their runtime behavior is unverified.

The internal mirror has neither required recipe. The approved public-source
fallback uses upstream archives with SHA-256 verification. This does not establish
that all transitive runtime libraries have passed a vulnerability review.

Sources: [Snowflake ODBC 4.0.0 release](https://github.com/snowflakedb/drivers/releases/tag/snowflake-odbc/4.0.0),
[unixODBC releases](https://www.unixodbc.org/download.html).

## Connector boundary

The raw connector owns its ODBC environment, connection and statements. It uses
the pinned 4.0.0 driver, HTTPS with certificate and hostname checks, and a PAT or
OAuth token read from `token_env`. `port` defaults to 443; `ca_file` optionally
selects a private CA bundle. There is no certificate-check bypass. Connection
fields are brace-quoted and embedded closing braces are escaped. Driver errors
expose SQLSTATE and native codes without copying a connection string or token
into the response.

The connector sets finite connection/query deadlines and one HTTP attempt. Each
statement requires a single SQL statement. BEGIN, COMMIT and ROLLBACK use the
same connection; finishing closes it. A missing COMMIT response remains an
error with an uncertain write outcome. The application does not reconnect or
replay that transaction. Destructor cleanup attempts rollback before disconnect.

Result transport preserves numeric text, binary bytes, NULL and empty values.
It reads truncated cells in chunks and refuses malformed length reports and
row/cell/result limits. Result-byte accounting includes value bytes and row and
column structures; it is not a process-memory quota or a bound on the driver's
prefetch allocations. Engine type conversion and server routing are not yet
implemented.

`tests/test_snowflake_transport.py` starts an owned HTTPS endpoint and runs
`snowflake_transport_probe` against it using the real driver. Its certificates
and fixture tokens are temporary. On Linux this runs as the
`snowflake_odbc_transport` CTest case. The API fault-injection cases in
`test_snowflake_connector.cpp` check handle cleanup, result limits and connection
lifetime. Neither suite substitutes for a live Snowflake acceptance run.
