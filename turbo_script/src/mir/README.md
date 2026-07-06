# MIR Backend Modules

This directory contains MIR backend code that has been split out of
`../turbo_script_mir.c`.

Current module ownership:

- `turbo_script_mir_internal.h`: shared internal MIR backend types and declarations.
- `turbo_script_mir_cache.c`: JIT cache helpers.
- `turbo_script_mir_compiler.c`: compiler state, register allocation, frame storage,
  pointer caches, and prologue/epilogue emission.
- `turbo_script_mir_constant.c`: compile-time constant folding helpers.
- `turbo_script_mir_oop.c`: OOP runtime bridges and OOP analysis helpers.
- `turbo_script_mir_runtime.c`: MIR external prototypes, external symbol loading,
  and general runtime bridge functions.
- `turbo_script_mir_value_runtime.c`: dynamic value evaluation bridges used when
  MIR lowering must preserve non-numeric values or execute AST interpreter paths.

`../turbo_script_mir.c` still owns the main AST lowering and compile/execute entry
points. Move additional code only as coherent ownership groups; avoid keeping
partially wired modules in the build.
