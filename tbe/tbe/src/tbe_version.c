#include "tbe_version.h"

const char *tbe_version(void) {
    return TBE_VERSION_STRING;
}

void tbe_version_components(int *major, int *minor, int *patch) {
    if (major) *major = TBE_VERSION_MAJOR;
    if (minor) *minor = TBE_VERSION_MINOR;
    if (patch) *patch = TBE_VERSION_PATCH;
}

int tbe_version_compatible(int required_major, int required_minor, int required_patch) {
    // For now, use simple semantic versioning rules:
    // - Major version must match exactly
    // - Minor version must be >= required
    // - Patch version is ignored for compatibility
    
    if (TBE_VERSION_MAJOR != required_major) {
        return 0;  // Major version mismatch
    }
    
    if (TBE_VERSION_MINOR < required_minor) {
        return 0;  // Minor version too old
    }
    
    // Patch version check is optional - usually newer patches are compatible
    // For strict compatibility checking, uncomment the next lines:
    // if (TBE_VERSION_PATCH < required_patch) {
    //     return 0;
    // }
    
    return 1;  // Compatible
}
