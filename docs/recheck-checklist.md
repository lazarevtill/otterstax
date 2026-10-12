# Production acceptance rechecks

**Release status: not production-ready.** This is the current verification
register for the [product decisions](product-decisions.md), not a claim that
every defect is known. A check closes only with observed evidence for the
candidate commit and its resolved dependency graph.

## Decision-sensitive checks

| Decision | Where to inspect / run | Required acceptance evidence | Current status |
|---|---|---|---|
| P1–P3: deployment and identity | `security/`, `frontend/`, `tests/test_client_security.py`, `tests/test_keycloak_live.py`, `docs/deployment.md` | Both modes on MySQL, PostgreSQL, Flight SQL and Spark; verified TLS; invalid, wrong-audience, expired and revoked tokens denied; provider outage denied; cleanup of an interrupted transaction. | 143 protocol security checks pass on the transaction baseline. The live Keycloak suite needs a run on the final candidate. |
| P2: tenant isolation | `component_manager/`, `main.cpp`, `tests/unit/utility/test_data_directory_lock.cpp`, file authorization system tests | Two instances cannot share a data directory; separate storage, files, credentials and audiences; restart cannot expose another instance's catalog or data. | Storage locking and file boundaries have tests. End-to-end two-instance acceptance remains required. |
| P4: dependency provenance | `conanfile.py`, `cmake/conan-recipes/`, Dockerfiles | Exact stable pins, internal-resolution result, approved fallback, artifact integrity, clean build and runtime library loading; vulnerability review. | New ODBC packages build locally. Complete dependency/CVE review and clean-image validation remain open. |
| P5–P6: transaction semantics | `frontend/common/sql_session.*`, `scheduler/`, `tests/test_native_transactions.py`, `tests/test_remote_transactions.py`, `tests/test_pg_transaction_batches.py` | Same backend session; visibility to a second client; rollback on statement failure/disconnect; PostgreSQL Flush/Sync/BEGIN behavior; independent concurrent transactions; no replay after a lost COMMIT response. | 14 native groups and 60 transaction groups pass; 16 existing raw PostgreSQL groups also pass. One-participant product fit still needs review. |
| P7: Describe fixture | `tests/test_pg_extended_protocol.py`, `tests/test_pg_transaction_batches.py` | Existing DML Describe assertions pass inside BEGIN/COMMIT; implicit portal expiry is separately asserted. | Passed on the transaction baseline. Preserve both sides of this contract. |
| P8: fault-injection observer | `tests/test_remote_transactions.py` | Snapshot refresh on each activity poll; the intended blocked query is observed and terminated; original deadlines and rollback assertions remain intact. | Passed against the live local PostgreSQL backend. |
| P11: MySQL fixture semantics | `tests/test_*client_*backend.py`, `tests/test_remote_transactions.py` | Independent operations explicitly autocommit; dedicated tests prove autocommit-off, isolation and rollback. | 39 backend integration groups pass with the explicit fixture settings. |
| P9–P10: Snowflake | Feature branch configuration tests; connector, integration actor, SQL generator and live fixture still required | Reads/INSERT/UPDATE/DELETE; exact NUMBER(38), NULL, binary and timestamp values; metadata; bounds; cancellation; verified TLS; session retention; rollback; lost connection and uncertain commit; implicit-commit DDL refused in a transaction. | Configuration tests pass. Datasource execution is unfinished. No test account exists, so live acceptance is blocked. |

## Verification baseline and limits

The checked reliability baseline is `19e3d6a`; its production C++ sources are
the same as `650afa8`. Observed results are 1,026/1,026 CTest cases, 39 backend
integration groups, 14 native transaction groups, 60 transaction groups,
16 existing raw PostgreSQL groups and 143 protocol security checks passing.
These counts cover different suites and are not a single unique-test total.
They do not validate subsequent feature changes.

The final candidate still needs the full Spark client matrix, Kafka crash
recovery, live Keycloak and S3 HTTPS suites. The ThreadSanitizer runtime failed
to start with an unexpected-memory-mapping error; it supplies no race-safety
evidence. Focused sanitizer results do not replace a complete sanitizer build
with a compatible engine ABI.

## Open reliability investigations

| Area | Evidence needed to close |
|---|---|
| Engine checkpoint after concurrent append rollback | Reproduce a non-tail aborted append, preserve another transaction's committed rows, prove checkpoint/WAL-floor advancement without restart, then crash/recover and verify both outcomes. Removing the tail guard is not a valid fix. |
| PostgreSQL prepared-statement lifecycle | Verify duplicate named statements/portals return protocol errors, parameter-count failures release allocated Worker state, close failures propagate, and unspecified parameter OIDs are inferred correctly. |
| Federated expression subqueries | Preserve remote routing through planning; prove correct rows and transaction-boundary enforcement. |
| Kafka durability and destinations | Prove broker offset/seek retry behavior and DDL atomicity under failures; enforce destination authorization for untrusted clients. |
| Backend security | Verified TLS and environment credential references for all relevant connectors, with rejection tests for certificate and hostname failures. |
| Resource limits | Bound query memory, decompression, result buffering, actor/thread count and cancellation cleanup under concurrent load. File-byte limits alone are insufficient. |
| Spark session settings | Verify SET/UNSET changes the promised behavior or reports unsupported, rather than acknowledging an ignored setting. |
| Error classification | Give request timeout and inactive engine transaction distinct observable outcomes. |
| Issue/PR coverage | Reconcile the current upstream issue and PR inventory with local reproductions, fixes and remaining gaps. A passing local patch is not an upstream resolution. |

For each candidate, record the commit, dirty-tree status, dependency revisions,
commands, exit codes and logs. Run targeted regressions for each fix, followed
by the suites affected by integration. Do not weaken assertions, hide tests or
count an unavailable live service as a pass. Document an uncertain write outcome
as uncertain; never retry it automatically merely to obtain a success response.
