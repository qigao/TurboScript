/* Real native module test: eager resolution must not rely on a host's
 * hidden static CSTL hash_bytes or on a second plugin owner.
 */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv) {
  void *handle;
  const char *error;
  if (argc != 2 || argv[1] == NULL) return 2;
  handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (handle == NULL) {
    error = dlerror();
    fprintf(stderr, "mapper dlopen failed: %s\n",
            error != NULL ? error : "unknown");
    return 1;
  }
  if (dlclose(handle) != 0) {
    fprintf(stderr, "mapper dlclose failed\n");
    return 1;
  }
  return 0;
}
