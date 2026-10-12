# Product decisions and acceptance boundaries

The fork separates product choices from reliability fixes and new features.
`feat/production-readiness` contains the audit fixes; `feat/snowflake-datasource`
contains the Snowflake feature; `feat/product-decisions` contains this decision
register and its [verification checklist](recheck-checklist.md).
These branches are reviewable work, not a production release.

## Decisions supplied by the owner

The question column identifies the choice asked about. An accepted direction
does not mean its implementation is complete or verified.

| ID | Question / choice | Owner's answer | Required behavior and recheck |
|---|---|---|---|
| P1 | Intended production access model | Both deployment modes | Support explicitly configured trusted-private access and Keycloak authentication. Recheck every frontend in both modes, including denial paths. |
| P2 | Shared tenant server or separate instances and storage | Separate server and storage per tenant | Separate process, persistent storage, connector credentials, network endpoint and identity audience for every tenant. Recheck isolation between two running instances; an identity accepted by one must not grant access to the other. |
| P3 | Production identity provider / credential source | Keycloak or similar | Keycloak is the implemented provider. Verify TLS, issuer, audience, expiry, revocation, provider outage and native/gRPC client behavior. Any accepted identity currently has access to the entire instance; per-user table grants are absent. |
| P4 | Internal mirror unavailable for authentication dependencies | Try the mirror, then public sources | Attempt internal resolution first. Public fallback is permitted, but each new pin still needs upstream stable-release verification, integrity checks and reproducible resolution. Package availability is not a vulnerability review. |
| P5 | Required transaction guarantee | Not specified | **Engineering decision requiring product review:** transactions currently use one local engine or one remote alias, with no distributed commit, savepoints or nested transactions. Confirm this boundary is adequate for the intended workloads before release. |
| P6 | Restrict remote protocol batches or implement remote transactions | Implement remote transaction support too | Retain the same backend session across statements, including PostgreSQL batches through `Sync`. Prove commit, rollback, error rollback, disconnect cleanup and uncertain commit outcomes. Reject cross-participant work before dispatch. |
| P7 | Correct PostgreSQL DML Describe fixture with explicit transaction boundaries | Add BEGIN/COMMIT | Keep portals alive across its two `Sync` batches through an explicit transaction. Preserve all existing assertions. Separately prove that an implicit transaction's portals expire. |
| P8 | Refresh PostgreSQL activity snapshot in the connection-loss observer | Refresh the snapshot | Refresh before each poll so the observer can see the blocked statement. Preserve the timeout and all termination/rollback assertions. |
| P9 | Snowflake datasource scope | Reads, writes and transactions | Full scope remains required. A dependency, configuration parser, mock or read-only connector does not complete it. Use a retained session and verify exact values, affected-row counts and rollback. |
| P10 | Availability of a Snowflake test account | No account | **Live Snowflake acceptance is unverified.** Offline tests cannot establish real authentication, permissions, session retention, warehouse behavior or transaction outcomes. |
| P11 | General MySQL fixtures rely on autocommit while their driver disables it | Set fixture autocommit explicitly | Declare autocommit in the independent-operation fixtures only. Dedicated transaction tests must continue to exercise autocommit-off, rollback and isolation. |
| P12 | Legacy scheduler-test engines share the default WAL/catalog directory | Apply isolated storage fixture | Give every test engine its own temporary persistent storage and clean it only after engine shutdown. Keep every assertion, persistence setting and parallel execution. Recheck the two conflicting cases together and the complete suite. |

The authorization to create a fork, push work and make focused commits applies
to these branches. Commits need plain descriptions without generated footers or
co-author trailers. It does not authorize merges, force pushes, deployment or
upstream comments.

## Implementation choices that are not owner-specified requirements

- The one-participant transaction contract, whole-block abort after an error,
  unsupported savepoints/options, and Flight SQL/Spark transaction limitations
  are defined in [transactions](transactions.md). Remote MySQL additionally
  requires proven InnoDB base tables without triggers. Review these compatibility
  limits against real clients; the request for remote transactions alone does
  not establish acceptance of every limitation.
- Keycloak uses online token introspection. Native clients supply access tokens
  through their password exchange over TLS; gRPC clients use bearer metadata.
  Recheck token refresh/reconnect behavior and identity-provider capacity. See
  [deployment](deployment.md) for the implemented contract.
- Snowflake implementation uses the official ODBC driver to retain a session.
  The feature branch includes dependency packaging, configuration validation,
  raw ODBC transport and owned HTTPS fixture coverage with the real driver.
  Executable datasource registration, engine conversion and query routing are
  not implemented yet. PAT/OAuth credentials via environment references are the
  current configuration scope. Account compatibility remains unverified.

No additional product approval is needed to run the listed local checks or to
continue implementing already requested work. New product questions should
identify a concrete behavior and trade-off, rather than ask for approval of an
unspecified implementation.
