# TurboScript Database Module

The `db` plugin is TurboScript's only generic database entry point.

It is backed by **TurboDB::Orm**. TurboScript does not choose a database
backend and does not load any driver implicitly.

## Import

```javascript
import("db");
```

## Explicit connection configuration

```javascript
conn = db.connect(map {
  driver: "sqlite",
  module_path: "/absolute/path/to/turbodb_driver_sqlite",
  options: map {
    filename: ":memory:"
  }
});
```

Both `driver` and `module_path` are required.

The `options` map is backend-specific and is passed to TurboDB unchanged.
For example, SQLite currently expects `filename`; other TurboDB drivers define
their own option keys.

There is no:

- default database;
- automatic driver-directory scan;
- driver alias inference;
- ABI fallback;
- fallback to SQLite;
- compatibility bridge to the removed `sqlite.*` TurboScript plugin.

The configured driver ID must match the loaded TurboDB Driver manifest.

## Commands

```javascript
affected = db.exec(
  conn,
  "INSERT INTO person(id,name) VALUES(?1,?2)",
  [7, "Alice"]
);
```

`db.exec` uses TurboDB's command CFlow Publisher:

```text
orm_raw
  -> orm_query_bind
  -> orm_query_open_command_flow
  -> CFlow demand
  -> affected_rows
```

Supported parameter values are null, bool, int64, finite number, string, and
bytes.

## Typed queries

```javascript
class Person {
  id: int64;
  name: string;
  score: double;
};

rows = db.query(
  conn,
  "SELECT id,name,score FROM person ORDER BY id",
  Person
);
```

Optional parameters are passed as a fourth argument:

```javascript
rows = db.query(
  conn,
  "SELECT id,name,score FROM person WHERE id > ?1",
  Person,
  [10]
);
```

Typed rows use the canonical TurboScript class CMeta/DataBind contract and
TurboDB's provider-backed object CFlow Publisher. TurboScript does not
materialize a generic ORM result and copy it afterward.

The third argument must be an actual class value, not a class-name string.

## Close

```javascript
db.close(conn);
```

A TurboScript `db` module context may load multiple explicit drivers and open
multiple connections. Reusing one driver ID with a different module path in the
same context fails immediately.

## Packaging

TurboScript CI restores the latest `TurboDB.Native` release. Consumers that
use `db` should also provide the current TurboDB package for their target
platform.

`db_tbs` links only `Orm::C`; it does not link SQLite, PostgreSQL, MySQL,
Redis, TidesDB, or another native database client directly.

The module is built only on platforms for which the current TurboDB package
publishes a CMake SDK. Absence of a TurboDB SDK does not trigger a fallback
database.
