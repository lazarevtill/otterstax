# Transactions

MySQL and PostgreSQL connections retain one engine session. `BEGIN`, `COMMIT`
and `ROLLBACK` operate on local Otterbrix tables in that session. Prepared
statements use the connection's transaction at execution time, including when
the statement was prepared before `BEGIN`. Local schema probes use that session
so they can describe tables created inside the transaction.

An error in a transaction aborts the whole block, including earlier statements.
Further queries fail until `ROLLBACK` or `COMMIT` ends the failed block. A
`COMMIT` of a failed block rolls back; PostgreSQL reports `ROLLBACK` in its
command tag. MySQL clients must also treat a statement error as aborting the
whole block, rather than assuming earlier statements remain committable.

MySQL `SET autocommit=0` starts a transaction at the next execution. `COMMIT`
and `ROLLBACK` end that transaction; another execution starts the next one.
Changing autocommit from off to on commits an active, successful transaction.
The OK and EOF status fields report the connection's actual transaction and
autocommit state.

Closing a connection rolls back its transaction and releases its prepared
statements. A request timeout closes the connection; Scheduler cleanup waits
for the outstanding request to finish before rolling back its engine session.
A timeout is not proof that an autocommit statement did not commit: the client
must reconcile its outcome before retrying a write.

## Supported boundary

Transactions cover local engine operations. Remote database execution, file
imports/exports, S3 operations and Kafka writes or DDL are rejected inside an
explicit local transaction. These resources do not participate in a distributed
commit protocol. Savepoints, nested transactions, transaction options and
two-phase commit are unsupported and return errors.

Flight SQL and Spark Connect use independent execution contexts. SQL transaction
commands through those paths are rejected. A native request must contain one
SQL statement.

PostgreSQL extended-protocol messages do not yet form an implicit atomic group
up to `Sync`. Use an explicit local transaction when grouping statements. This
is a PostgreSQL protocol compatibility gap and a production-readiness blocker
for clients that rely on pipeline atomicity.
