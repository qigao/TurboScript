# RulesForge Module (`rules_forge`)

The `rules_forge` plugin exposes the RulesForge C API to TurboScript through a
handle-based module namespace.

## Dependency Direction

`rules_forge_plugin` links to `RulesForge::rule_forge`. RulesForge uses DataBind
for schema-bound fact input. `tbe/data_bind` does not depend on RulesForge.

RulesForge's C++ RHS adapter is internal to RulesForge. It loads the
TurboScript runtime through RulesForge's runtime loader and does not require
TurboScript modules to expose RulesForge C++ types.

## Loading

```javascript
import("rules_forge");
```

RulesForge is linked through `RulesForge::rule_forge` and loaded as a normal runtime
dependency. Ensure the RulesForge runtime library is available in your process
runtime search path for deployment.

## Knowledge Base

```javascript
let kb = rules_forge.kb_create();
let status = rules_forge.kb_load(kb, rules_text);
status = rules_forge.kb_load_file(kb, "rules/order.rfl");
status = rules_forge.kb_load_decision_table_csv(kb, csv_text);
rules_forge.kb_destroy(kb);
```

## Sessions and Facts

```javascript
let session = rules_forge.session_create(kb);

let bound = rules_forge.session_add_fact_json_schema(
    session, "schemas/order.tbe", "Order", json_text);
let first = rules_forge.session_add_fact_json_path_schema(
    session, "schemas/order.tbe", "Order", envelope_json, "$.orders[*]");
let selected = rules_forge.session_add_facts_json_path_schema(
    session, "schemas/order.tbe", "Order", envelope_json, "$.orders[*]");
let yaml_bound = rules_forge.session_add_fact_yaml_schema(
    session, "schemas/order.tbe", "Order", yaml_text);
let yaml_first = rules_forge.session_add_fact_yaml_path_schema(
    session, "schemas/order.tbe", "Order", envelope_yaml, "/orders/*");
let yaml_selected = rules_forge.session_add_facts_yaml_path_schema(
    session, "schemas/order.tbe", "Order", envelope_yaml, "/orders/*");
let bound = rules_forge.session_add_fact_json_schema_path(
    session, "schemas/order.tbe", "Order", "fixtures/order.json");

let csv_result = rules_forge.session_add_facts_csv_schema(
    session, "schemas/order.tbe", "Order", csv_text);
let west_orders = rules_forge.session_add_facts_csv_path_schema(
    session, "schemas/order.tbe", "Order", csv_text, "region == \"west\"");
let csv_result = rules_forge.session_add_facts_csv_schema_path(
    session, "schemas/order.tbe", "Order", "fixtures/orders.csv");
let xml_result = rules_forge.session_add_facts_xml_schema(
    session, "schemas/order.tbe", "Order", xml_text, "/orders/order");
let xml_result = rules_forge.session_add_facts_xml_schema_path(
    session, "schemas/order.tbe", "Order", "fixtures/orders.xml", "/orders/order");

let fired = rules_forge.session_fire_all(session, -1);
rules_forge.session_destroy(session);
```

`session_fire_all()` returns an object:

```javascript
{ status: 0, fired: 3 }
```

Multi-fact JSONPath, YAML/YPATH, CSV/CSVPath, and XML insertion returns:

```javascript
{ status: 0, loaded: 2, facts: [0, 1] }
```

Schema-bound JSON and YAML facts preserve the complete DataBind value model.
RulesForge converts object/composite, list, set, map, bytes, UUID, datetime,
date, time, duration, decimal, bigint, and money fields into its owned fact
values before the DataBind parse tree is released. UUID and temporal values
are normalized for rule comparisons; decimal, bigint, and money comparisons
retain their canonical text precision.

RulesForge 0.5 requires schema-bound structured input. The previous
`session_add_fact_json`, `session_add_facts_csv`, and `kb_load_ts_plugin`
entry points are no longer exposed because the upstream C API removed them.

## Stateful DataBind Streams

Create an incremental stream, feed memory chunks or a file, finish the batch,
then destroy the stream handle:

```javascript
let input = rules_forge.stream_json_all_create(
    session, "schemas/order.tbe", "Order");
rules_forge.stream_feed(input, json_chunk_1);
rules_forge.stream_feed(input, json_chunk_2);
let loaded = rules_forge.stream_finish(input);
rules_forge.stream_destroy(input);
```

Constructors are available for JSON and YAML root/all/path/path-all, CSV
all/path, and XML root/path-all. YAML path constructors use YPATH expressions
such as `/orders/*`. `stream_finish` returns the same
`{ status, loaded, facts }` shape as synchronous batch insertion. Streams are
owned by their stateful session and must be used on the same thread.

## Continuous Sessions

Continuous sessions process schema-bound events with bounded event-time state:

```javascript
let continuous = rules_forge.continuous_create(kb, {
    max_active_events: 10000,
    allowed_lateness_ms: 5000,
    output_fact_types: ["Alert"]
});
let step = rules_forge.continuous_push_json_schema(
    continuous, "schemas/order.tbe", "Order", "event-1", "orders",
    1720000000000, json_text);
let metrics = rules_forge.continuous_metrics(continuous);
rules_forge.continuous_acknowledge(continuous, step.batch_id);
rules_forge.continuous_result_destroy(step.result);
rules_forge.continuous_destroy(continuous);
```

Path-selected JSON, YAML, CSV, and XML batches read event metadata from bound fields:

```javascript
let step = rules_forge.continuous_push_json_path_schema(
    continuous, "schemas/event.tbe", "Event", envelope_json, "$.events[*]",
    "event_id", "event_time", "events");

let input = rules_forge.continuous_stream_json_path_create(
    continuous, "schemas/event.tbe", "Event", "$.events[*]",
    "event_id", "event_time", "events");
rules_forge.continuous_stream_feed(input, chunk);
let streamed_step = rules_forge.continuous_stream_finish(input);
rules_forge.continuous_stream_destroy(input);

let yaml_step = rules_forge.continuous_push_yaml_path_schema(
    continuous, "schemas/event.tbe", "Event", envelope_yaml, "/events/*",
    "event_id", "event_time", "events");

let yaml_input = rules_forge.continuous_stream_yaml_path_create(
    continuous, "schemas/event.tbe", "Event", "/events/*",
    "event_id", "event_time", "events");
```

YAML selectors use YPATH syntax such as `/events/*`, not JSONPath syntax.
Equivalent `continuous_push_csv_path_schema`, `continuous_push_xml_path_schema`,
`continuous_stream_csv_path_create`, and `continuous_stream_xml_path_create`
functions use the same metadata-field and entry-point arguments. Root YAML
events use `continuous_push_yaml_schema` or `continuous_stream_yaml_create`.

The config starts from RulesForge defaults; supplied fields override only the
documented bounded values. Push, watermark, drain, and continuous stream finish
return an object containing `status`, `result`, `step_status`, `batch_id`,
`rules_fired`, `events_expired`, watermark fields, and `output_count`.

`continuous_result_output(result, index)` returns a fact handle borrowed from
that result. Destroying the result invalidates all such handles. Continuous
JSON and YAML streams use `continuous_stream_json_create` or
`continuous_stream_yaml_create`, followed by `continuous_stream_feed`,
`continuous_stream_feed_file`, `continuous_stream_finish`, and
`continuous_stream_destroy`.

DataBind callbacks are synchronous and records are borrowed only for callback
duration. RulesForge copies each callback record into its own pending event;
`feed` never mutates the continuous session. `continuous_stream_finish`
atomically submits the complete pending batch. A failed feed or invalid metadata
clears the pending batch, so subsequent finish cannot partially commit it.
Files, sockets, HTTP clients, and brokers remain external byte-chunk producers;
neither DataBind nor this plugin creates a thread, event loop, or network task.

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

The RulesForge 0.5 C ABI currently exposes field accessors only for string,
double, int64, and bool. Complex fields are available to RulesForge rules but
are not exported as raw `DataBindValue` pointers or TurboScript values. This
keeps fact ownership inside the session/query boundary; no plugin-side clone or
cross-module free is required.

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
