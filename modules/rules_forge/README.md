# RulesForge Module (`rules_forge`)

The `rules_forge` plugin exposes the RulesForge C API to TurboScript through a
handle-based module namespace.

## Dependency Direction

`rules_forge_plugin` links to `RulesForge::RulesForge`. RulesForge may link to
`TurboScript::DataBind` and may dynamically load TurboScript plugins through
`ruleforge_kb_load_ts_plugin()`. `tbe/data_bind` does not depend on RulesForge.

RulesForge's C++ RHS adapter is internal to RulesForge. It loads the
TurboScript runtime through RulesForge's runtime loader and does not require
TurboScript modules to expose RulesForge C++ types.

## Loading

```javascript
import("rules_forge");
```

On Windows the plugin build copies `rule_forge.dll` and `data_bind.dll` beside
`tbs_rules_forge.dll`.

## Knowledge Base

```javascript
let kb = rules_forge.kb_create();
let status = rules_forge.kb_load(kb, rules_text);
status = rules_forge.kb_load_file(kb, "rules/order.rfl");
status = rules_forge.kb_load_decision_table_csv(kb, csv_text);
status = rules_forge.kb_load_ts_plugin(kb, "math_ext_plugin.dll");
rules_forge.kb_destroy(kb);
```

`rules_forge.kb_load_ts_plugin(kb, path)` delegates to RulesForge and registers
TurboScript/exprtk plugin functions for RulesForge RHS invocation.

## Sessions and Facts

```javascript
let session = rules_forge.session_create(kb);

let fact = rules_forge.session_add_fact_json(
    session, "Order", "{\"qty\": 10, \"active\": true}");

let bound = rules_forge.session_add_fact_json_schema(
    session, "schemas/order.tbe", "Order", json_text);

let csv_result = rules_forge.session_add_facts_csv_schema(
    session, "schemas/order.tbe", "Order", csv_text);

let xml_result = rules_forge.session_add_facts_xml_schema(
    session, "schemas/order.tbe", "Order", xml_text, "/orders/order");

let fired = rules_forge.session_fire_all(session, -1);
rules_forge.session_destroy(session);
```

`session_fire_all()` returns an object:

```javascript
{ status: 0, fired: 3 }
```

CSV/XML batch insertion returns:

```javascript
{ status: 0, loaded: 2, facts: [0, 1] }
```

## Queries and Field Access

```javascript
let query = rules_forge.session_query(session, "eligible_orders");
let size = rules_forge.query_size(query);
let fact = rules_forge.query_fact(query, 0, "$order");

let id = rules_forge.fact_int(fact, "id");
let qty = rules_forge.fact_double(fact, "qty");
let side = rules_forge.fact_string(fact, "side");
let active = rules_forge.fact_bool(fact, "active");

rules_forge.query_destroy(query);
```

Fact handles are borrowed from the owning session or query. Destroying or
resetting a session invalidates its fact handles. Destroying a query invalidates
facts borrowed from that query.

## Observability

```javascript
rules_forge.session_enable_tracing(session, 1);
let trace = rules_forge.session_trace(session, 0);
let perf = rules_forge.session_rule_performance(session);
let mem = rules_forge.session_memory_stats(session);
rules_forge.session_clear_trace(session);
```

## Errors

Most RulesForge C API wrappers return the underlying status code. Handle-creating
functions return `-1` on failure. Use `rules_forge.error()` to retrieve the last
module error string.

```javascript
let status = rules_forge.kb_load(99, "bad");
if (status != 0) {
    print(rules_forge.error());
}
```
