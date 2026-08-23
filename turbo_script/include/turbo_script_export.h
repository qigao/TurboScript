#ifndef TURBO_SCRIPT_EXPORT_H
#define TURBO_SCRIPT_EXPORT_H

#ifndef TURBO_SCRIPT_API
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(TURBO_SCRIPT_BUILD_SHARED)
#define TURBO_SCRIPT_API __declspec(dllexport)
#elif defined(TURBO_SCRIPT_USE_SHARED)
#define TURBO_SCRIPT_API __declspec(dllimport)
#else
#define TURBO_SCRIPT_API
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define TURBO_SCRIPT_API __attribute__((visibility("default")))
#else
#define TURBO_SCRIPT_API
#endif
#endif

#ifdef __cplusplus
#define TURBO_SCRIPT_C_API extern "C" TURBO_SCRIPT_API
#else
#define TURBO_SCRIPT_C_API TURBO_SCRIPT_API
#endif

#endif /* TURBO_SCRIPT_EXPORT_H */
