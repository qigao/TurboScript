# Research playbooks

The files in this directory are research specifications adapted from an
external quantitative playbook. They are not standalone TurboScript examples.

They assume that the embedding host provides:

- a `finance` adapter with the named factor and portfolio functions;
- market-data variables such as `CLOSE`, `RETURNS` and `INDEX_RETURNS`;
- trading callbacks such as `buy`, `flat`, `is_long` and `is_flat`.

The built-in plugins are instead loaded as `fin`, `ts` and `ta`, exposing the
`strategy.*`, `ts.*` and `ta.*` namespaces. Those plugins do not currently
provide every adapter function referenced by these research specifications.
Use the executable scripts under `examples/` for acceptance testing.
