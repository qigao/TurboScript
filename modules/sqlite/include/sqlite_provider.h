/**
 * @file sqlite_provider.h
 * @brief Canonical CMeta interface for the SQLite stateful provider.
 *
 * The provider owns native database/session semantics only. It deliberately
 * exposes no ExprTk/TurboScript value types.
 */
#ifndef TURBOSCRIPT_SQLITE_PROVIDER_H
#define TURBOSCRIPT_SQLITE_PROVIDER_H

#include <cmeta/interface.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SQLITE_PROVIDER_EXPORT_ID "sqlite.provider"
#define SQLITE_PROVIDER_CONTRACT_ID "turboscript.sqlite.provider"
#define SQLITE_PROVIDER_CONTRACT_VERSION 1u

typedef struct sqlite_provider_f64_column_s {
    double *data;
    size_t count;
} sqlite_provider_f64_column;

/*
 * The exported interface is a provider/factory handle. create() returns one
 * per-TurboScript-context session; all remaining operations receive that
 * session explicitly. This preserves context isolation even when a platform
 * loader maps the same DSO only once per process.
 *
 * error() returns a borrowed provider string valid until the session is
 * destroyed or the handle error is replaced by a later database operation.
 *
 * query_column() allocates provider-owned output storage. The caller must
 * release it with release_column().
 */
#define SQLITE_PROVIDER_METHODS(X, I)                                         \
    X(I, R0, void *, create, _)                                               \
    X(I, V1, void, destroy, void *, session)                                  \
    X(I, R2, int, open, void *, session, const char *, path)                  \
    X(I, V2, void, close, void *, session, int, handle)                       \
    X(I, R3, int, exec, void *, session, int, handle, const char *, sql)      \
    X(I, R4, bool, query_scalar, void *, session, int, handle,                \
      const char *, sql, double *, out_value)                                 \
    X(I, R4, bool, query_column, void *, session, int, handle,                \
      const char *, sql, sqlite_provider_f64_column *, out_column)            \
    X(I, V2, void, release_column, void *, session,                           \
      sqlite_provider_f64_column *, column)                                   \
    X(I, R2, const char *, error, void *, session, int, handle)

CMETA_INTERFACE(sqlite_provider, SQLITE_PROVIDER_METHODS);

sqlite_provider *sqlite_provider_export(void);

#ifdef __cplusplus
}
#endif

#endif /* TURBOSCRIPT_SQLITE_PROVIDER_H */
