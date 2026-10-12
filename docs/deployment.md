# Deployment boundaries

Each tenant needs a separate server process, data directory, connector
configuration and network endpoint. The scheduler and catalog do not enforce
permissions between tenants inside one process.

## Storage

The storage directory comes from `--data-dir`, then `OTTERSTAX_DATA_DIR`, then
`./data`. It contains engine data, WAL and logs. Docker images set
`OTTERSTAX_DATA_DIR=/app/data`; mount the tenant's persistent volume there.

The process acquires an exclusive OS lock on `.otterstax.lock` before starting
the engine or loggers. Another process using the same directory fails startup.
The OS releases ownership when the process exits, including after a crash.
The lock file remains in place; do not delete or replace it while a server runs.

The engine is built from the pinned recipe in `cmake/conan-recipes/otterbrix`.
Its source patches establish a durable empty table root before write-through
and drain transaction undo after failed DML. The application keeps ingested rows
and partition checkpoints in one engine transaction. Kafka recovery errors
prevent listeners from opening; an absent Kafka registry is a normal first start.

## Listeners

MySQL and PostgreSQL accept `service.mysql.host` and `service.postgres.host`
as IPv4 or IPv6 addresses. Their default is `0.0.0.0`. Flight SQL and Spark
Connect use their respective `service.flight_sql.host` and
`service.spark_connect.host` settings. Configure each tenant's listeners and
network access separately.

## Client identity

`service.security.mode` is required at startup. `trusted_private` admits clients
without checking credentials and requires an access-controlled network. The
checked-in demo, benchmark and test configurations explicitly select this mode.
It can use TLS by configuring both `certificate_file` and `private_key_file`.

`keycloak` requires those TLS files and a confidential Keycloak client. Each
tenant instance needs its own client audience. Configure the audience mapper in
Keycloak to include that client ID in access tokens for authorized clients.

```yaml
service:
  security:
    mode: keycloak
    certificate_file: /run/tls/server.crt
    private_key_file: /run/tls/server.key
    keycloak:
      issuer: https://identity.example/realms/tenant
      client_id: tenant-database
      client_secret_env: OTTERSTAX_KEYCLOAK_SECRET
      ca_file: /run/tls/identity-ca.crt
      request_timeout_ms: 5000
      max_token_bytes: 16384
      max_response_bytes: 65536
      max_in_flight: 16
```

The secret is read from the named environment variable, never from a literal
YAML credential. `ca_file` is optional when the provider uses the system trust
store. The issuer must be an HTTPS realm URL without a trailing slash. The
server calls its Keycloak token introspection endpoint over verified HTTPS,
with no redirects or positive-result cache. It requires an active Bearer access
token, exact issuer, the instance's audience, a subject and a future expiration;
future `nbf` values are refused. Invalid tokens and provider failures deny access.

Clients acquire access tokens from Keycloak themselves. The database does not
exchange user passwords for tokens:

- Flight SQL and Spark Connect send `authorization: Bearer <access-token>` on
  every RPC over TLS, including ticket, session and data operations.
- MySQL negotiates TLS, then responds to the `mysql_clear_password` authentication
  switch with the access token. The client must enable that standard plugin and
  verify the server certificate and hostname.
- PostgreSQL negotiates TLS and supplies the access token as its password in the
  standard PasswordMessage exchange. Use `sslmode=verify-full` with the appropriate
  trust root. This is token authentication through PasswordMessage, not SASL
  OAUTHBEARER or a Keycloak password grant.

Native connections revalidate their token before each command/message; reconnect
with a fresh token after expiry or revocation. Each gRPC call validates its own
metadata. An accepted operation may finish after its token expires. Native
introspection occupies one connection executor thread until the configured HTTP
timeout; the provider concurrency cap applies across all four frontends.

An accepted identity has access to the entire tenant instance, its catalog and
configured backends. There are no per-user SQL roles or table grants. User-specified
Kafka destinations still require an authorization boundary before admitting
untrusted tenant operators.

`tests/test_client_security.py` exercises the actual server protocols against an
HTTPS introspection fixture. `tests/test_keycloak_live.py` owns a separate local
Keycloak container for live signature, audience and expiry checks. Both generate
temporary credentials and certificates and require an explicitly supplied server
binary; neither targets an existing production identity provider.
