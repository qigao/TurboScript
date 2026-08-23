#ifndef EXPRTK_EXPORT_H
#define EXPRTK_EXPORT_H

#ifndef EXPRTK_API
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(EXPRTK_BUILD_SHARED)
#define EXPRTK_API __declspec(dllexport)
#elif defined(EXPRTK_USE_SHARED)
#define EXPRTK_API __declspec(dllimport)
#else
#define EXPRTK_API
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define EXPRTK_API __attribute__((visibility("default")))
#else
#define EXPRTK_API
#endif
#endif

#ifdef __cplusplus
#define EXPRTK_C_API extern "C" EXPRTK_API
#else
#define EXPRTK_C_API EXPRTK_API
#endif

#endif /* EXPRTK_EXPORT_H */
