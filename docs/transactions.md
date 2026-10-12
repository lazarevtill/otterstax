# Transactions

MySQL and PostgreSQL client connections retain one execution session. `BEGIN`,
`COMMIT` and `ROLLBACK` cover local Otterbrix tables or one remote PostgreSQL or
MySQL connection alias. Prepared statements use the connection's transaction at execution time, including when
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
for the outstanding request to finish before rolling back its storage session.
A timeout is not proof that an autocommit statement did not commit: the client
must reconcile its outcome before retrying a write.

## Supported boundary

The first storage operation selects the transaction's participant. Every later
operation must use that local engine or remote alias. File imports/exports, S3,
Kafka writes or DDL, ClickHouse and operations spanning participants are rejected
before dispatch. There is no distributed commit protocol. Savepoints, nested
transactions, transaction options and two-phase commit return errors.

Remote transactions support SELECT, INSERT, UPDATE and DELETE. Each owns a
dedicated connector and integration actor. Its socket is never reconnected or
returned to the autocommit connection. A blocked remote query does not hold the
actor another transaction needs for rollback. A lost COMMIT response remains
an uncertain outcome; the server closes that socket and does not retry.

MySQL transactions require InnoDB base tables without triggers. The server
acquires metadata locks through zero-row probes and checks table capabilities
before executing user statements. The backend account must have a direct
TRIGGER grant covering the tables so hidden triggers cannot look like an empty
trigger list. Unproven role grants, partial revokes, views, other storage
engines and raw subquery slots without complete table dependencies are refused.
Remote expression subqueries are unsupported in transactions.

Flight SQL and Spark Connect use independent execution contexts. SQL transaction
commands through those paths are rejected. A native request must contain one
SQL statement.

PostgreSQL extended-protocol messages do not yet form an implicit atomic group
up to `Sync`. Use an explicit transaction when grouping supported statements. This
is a PostgreSQL protocol compatibility gap and a production-readiness blocker
for clients that rely on pipeline atomicity.
