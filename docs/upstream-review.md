# Upstream issue and PR review

The upstream inventory contains two open issues and two open pull requests.
The local audit does not close or merge those items. Reliability fixes are on
`feat/production-readiness`; product decisions and acceptance gaps are tracked
in the [recheck checklist](recheck-checklist.md).

| Item | Local disposition | Recheck before release |
|---|---|---|
| [Issue 34: Kafka ordering and retry invariants](https://github.com/otterstax/otterstax/issues/34) | Commit `1964ad8` retains failed source batches before polling more records; batch-loop regressions cover sequencing and retries. | Rerun crash recovery against the final candidate; verify no offset advances past failed work in both ingestion modes. |
| [Issue 35: unbounded source offsets](https://github.com/otterstax/otterstax/issues/35) | Commit `8d607a5` replaces a partition checkpoint inside the data transaction. | Prove one row per partition after repeated batches and correct crash recovery. Engine checkpoint advancement after a concurrent rollback remains a separate open investigation. |
| [PR 31: PostgreSQL startup overflow](https://github.com/otterstax/otterstax/pull/31) | The local frontend validates the unsigned announced size before subtraction/allocation, grows the buffer within its cap, and reports the announced length in the error. Raw-wire malformed-packet regressions cover startup input. | Include these tests in the final build and sanitizer run; a clean functional run is not memory-safety evidence from a sanitizer. |
| [PR 36: PR replica workflow](https://github.com/otterstax/otterstax/pull/36) | Reviewed at `c7edf84e9b3775259e10f1d1746cb2bc659282c5`. The workflow has independent recovery, failure-isolation and concurrent-update defects. A local correction is available as a review patch; it is not an active workflow in the fork. | Validate repository/environment settings and delivery authorization before enabling replication. Local mocks do not establish a successful remote synchronization. |

## PR 36 findings

- **High:** an existing destination branch causes an ordinary retry to skip PR
  reconciliation. A push that succeeds before PR creation fails leaves no way
  to repair the missing PR without choosing branch overwrite. Branch existence
  and PR existence need independent reconciliation, including closed/merged PRs.
- **Medium:** a command/transport failure under `set -e` terminates the batch,
  leaving subsequent merged PRs unprocessed. Each item needs an error boundary,
  with an overall failure after every selected item is attempted.
- **Medium:** concurrent runs and manual updates can race an unconditional force
  push. A requested replacement must use a lease against the observed remote
  commit. Force permission cannot be inferred from a failed API request.

The [review patch](reviews/pr36-corrections.patch) and its nine isolated tests cover partial-success repair, preserving
closed copies, pagination, leases and continuation after an item fails. Patch
application is checked against the stated PR commit. No synchronization workflow,
remote PR creation or remote comment is part of this verification.
