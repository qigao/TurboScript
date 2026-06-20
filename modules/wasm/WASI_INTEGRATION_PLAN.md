# Wasm3 and uvwasi Integration Plan for tScript

Based on the capabilities of `vendor/wasm3` and `vendor/uvwasi`, we can significantly enhance `tScript/modules/wasm` to go beyond simple computational WebAssembly functions and support **fully-fledged system-level Wasm applications** (like those compiled from Rust, C/C++, or Go targeting `wasm32-wasi`).

## 1. What Can We Do?

### A. Provide WebAssembly System Interface (WASI) Support
Currently, `tScript/modules/wasm` can load `.wasm` files and call functions natively through `turbo_wasm`. However, the WebAssembly module operates in a closed sandbox without access to the host's operating system (e.g., standard streams, files, clocks, environment variables).
By integrating `uvwasi`, we can provide a compliant WASI environment (specifically `wasi_snapshot_preview1`), allowing Wasm code to:
- Write to `stdout` and `stderr` (e.g., `printf` in C or `println!` in Rust).
- Access pre-opened directories mapping a virtual sandboxed path to a physical disk path securely.
- Query CLI arguments and environment variables passed from TurboScript.
- Poll times and clocks securely.

### B. Adopt Advanced Memory Management (Arena)
`wasm3` natively supports Arena Memory Management. Within the `exprtk_mod_wasm.c` bindings, `turbo_arena` is already heavily used. Integrating `wasm3`'s thread arena bindings directly (`m3_SetThreadArena()`) can yield zero memory fragmentation and provide an instant $O(1)$ cleanup when `wasm.close()` is called.

### C. Enhanced TurboScript/exprtk Wasm API
We can introduce new `exprtk` functions to dynamically control the WASI sandbox per-module:
- `wasm.wasi_preopen(handle, dir_mapped, dir_real)`
- `wasm.wasi_env(handle, key, value)`
- `wasm.wasi_args(handle, "arg1", "arg2")`

## 2. How to Implement It

To implement this, we need to bridge `tScript` ↔ `uvwasi` ↔ `wasm3` (via the `turbo_wasm` abstraction).

### Step 1: Update the Build System (`CMakeLists.txt`)
Modify `tScript/modules/wasm/CMakeLists.txt` to include `uvwasi`. Since `uvwasi` is in `vendor/uvwasi`, it must be added to the project targets and linked to `wasm_plugin` / `wasm_plugin_lib`.

```cmake
# In tScript/modules/wasm/CMakeLists.txt
target_include_directories(${TARGET_NAME_LIB} PUBLIC ${CMAKE_SOURCE_DIR}/vendor/uvwasi/include)
target_link_libraries(${TARGET_NAME_LIB} PRIVATE uvwasi uv) # Assuming uvwasi builds alongside libuv
```

### Step 2: Extend the Wasm Handle Context (`wasm_ctx.h`)
We need to hold the `uvwasi_t` state within the handle, alongside the `turbo_wasm_vm_t`.

```c
#include "uvwasi.h"

typedef struct {
  turbo_wasm_vm_t *vm;
  uvwasi_t uvw; // Store uvwasi context for the lifecycle of the VM
  bool wasi_initialized;
  char error_msg[256];
} wasm_handle_t;
```

### Step 3: Implement Configuration lifecycle in `exprtk_mod_wasm.c`
Currently, `wasm.open` allocates a VM and immediately loads a `.wasm` file. Because `uvwasi` options must be initialized before the Wasm module runs, we might want to expose a staged initialization or pass flags, but fundamentally we must connect them.

```c
// Setup uvwasi options
uvwasi_options_t init_options = {0};
init_options.in = 0;
init_options.out = 1;
init_options.err = 2;
init_options.fd_table_size = 3;
// Setup argc, argv, envp, preopens here as needed...

// Initialize rust-like sandbox
uvwasi_errno_t err = uvwasi_init(&h->uvw, &init_options);
if (err != UVWASI_ESUCCESS) {
    // Handle Error
}
h->wasi_initialized = true;
```

### Step 4: Link `uvwasi` to the `wasm3` Environment runtime
`turbo_wasm_vm_t` will need a new API to link the WASI symbols to the `uvwasi` backend callbacks. Because `wasm3` requires defining raw host functions, you will likely implement wrappers in `turbo_wasm` that map `wasi_snapshot_preview1` system calls (like `fd_write`, `environ_sizes_get`) directly to their corresponding `uvwasi_fd_write`, `uvwasi_environ_sizes_get`, passing `&h->uvw` as the primary user-data argument. 

```c
// Example pseudo-code inside turbo_wasm or wasm_plugin:
m3_LinkRawFunction(module, "wasi_snapshot_preview1", "fd_write", "i(i*ii)",
                   (M3RawCall)&wasi_proxy_fd_write);

// Proxy function
m3ApiRawFunction(wasi_proxy_fd_write) {
    m3ApiGetArg(int, fd);
    m3ApiGetArgMem(uvwasi_iovec_t *, iovs);
    m3ApiGetArg(int, iovs_len);
    m3ApiGetArgMem(size_t *, nwritten);
    
    uvwasi_t* uvw = (uvwasi_t*)_ctx->userdata;
    uvwasi_errno_t res = uvwasi_fd_write(uvw, fd, iovs, iovs_len, nwritten);
    m3ApiReturn(res);
}
```
*(Note: If `wasm3` comes with its own `m3_LinkWASI` that integrates out-of-the-box with `uvwasi`, you can simply pass the `uvwasi_t*`!)*

### Step 5: Clean Up
In `wasm_handle_free()`, remember to destroy the WASI context:
```c
if (h->wasi_initialized) {
    uvwasi_destroy(&h->uvw);
}
```

## Summary architecture
1. **TurboScript (`exprtk_mod_wasm`)** receives command: `wasm.open(...)`.
2. **`wasm.open`** initializes `uvwasi_t` sandbox, dictating how the file descriptors and env logic maps to the physical system.
3. **`turbo_wasm`** loads WebAssembly. Symbols are linked to invoke `uvwasi` functions.
4. **`wasm.call`** starts WASI execution inside `wasm3`, gracefully routing syscalls like I/O back out through `libuv`.
