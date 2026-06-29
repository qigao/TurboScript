# SQLite Module (`sqlite`)

The `sqlite` plugin exposes a small SQLite API to TurboScript and includes
helpers for local RAG storage:

- generic SQL execution and scalar/vector query helpers
- FTS5 availability probing
- dense embedding storage as SQLite BLOB values
- cosine similarity search over embedding tables
- a compact `rag_*` helper set combining FTS5 and embedding scores

## Build

FTS5 requires SQLite to be built with `SQLITE_ENABLE_FTS5`. The repository
declares this through `sqlite3[fts5]` in `vcpkg.json`. If an existing
`vcpkg_installed` tree was created before that feature was enabled, rebuild or
reinstall the sqlite3 package.

```bat
vcpkg install sqlite3[fts5]:x64-windows
cmake --preset win-dev-user
cmake --build build/Msvc --target sqlite_plugin test_sqlite_module
```

Runtime probing:

```javascript
let ok = sqlite.fts5_available();
```

## Basic SQL

```javascript
import("sqlite");

let db = sqlite.open(":memory:");
sqlite.exec(db, "CREATE TABLE prices(id INTEGER, value REAL)");
sqlite.exec(db, "INSERT INTO prices VALUES (1, 42.0)");

let value = sqlite.query_scalar(db, "SELECT value FROM prices WHERE id = 1");
let col = sqlite.query_col(db, "SELECT value FROM prices", 0);
sqlite.close(db);
```

## Embedding Tables

Embeddings are stored as raw `double` arrays in BLOB columns. This is intended
for local TurboScript databases; use a portable encoding if the database must be
shared across architectures.

```javascript
let db = sqlite.open("rag.db");
sqlite.embedding_init(db, "embeddings");

sqlite.embedding_put(db, "embeddings", 1, [1.0, 0.0, 0.0]);
sqlite.embedding_put(db, "embeddings", 2, [0.0, 1.0, 0.0]);

let rows = sqlite.embedding_search(db, "embeddings", [0.9, 0.1, 0.0], 5);
// rows: [{ id, score }, ...]
```

Vector helpers:

```javascript
let blob = sqlite.vec_blob([1.0, 2.0, 3.0]);
let vec = sqlite.vec_from_blob(blob);
let score = sqlite.vec_cosine([1, 0], [0.5, 0.5]);
```

## RAG Helper

`sqlite.rag_init(db)` creates three fixed tables:

- `rag_docs(id INTEGER PRIMARY KEY, source TEXT, text TEXT)`
- `rag_fts`, an FTS5 table over document text
- `rag_embeddings(doc_id INTEGER PRIMARY KEY, dim INTEGER, embedding BLOB)`

```javascript
let db = sqlite.open("rag.db");
sqlite.rag_init(db);

sqlite.rag_add(
    db, 1, "guide.md",
    "SQLite FTS5 supports full text search",
    [1.0, 0.0, 0.0]);

let hits = sqlite.rag_search(
    db,
    "SQLite full text",
    [0.9, 0.1, 0.0],
    10,
    1.0,  // FTS weight
    1.0); // vector weight
```

`sqlite.rag_search()` returns a list of objects:

```javascript
{
  id: 1,
  score: 1.94,
  vector_score: 0.99,
  fts_score: 0.95,
  source: "guide.md",
  text: "SQLite FTS5 supports full text search"
}
```

The embedding score is cosine similarity. The FTS score is derived from
`bm25(rag_fts)` and normalized so higher is better.
