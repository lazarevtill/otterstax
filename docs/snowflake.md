# Snowflake datasource status

Snowflake datasource execution is not available yet. This feature branch contains
the driver dependencies and configuration validation; it does not register a
queryable backend. Reads, writes and retained transactions remain the required
feature scope. No live Snowflake account is available for acceptance testing.

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
