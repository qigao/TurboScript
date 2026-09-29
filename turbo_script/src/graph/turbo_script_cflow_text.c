#include "turbo_script_cflow_text.h"

#include <stdlib.h>
#include <string.h>

static const cmeta_type_traits ts_cflow_line_slice_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY
};

static const cmeta_type_desc ts_cflow_line_slice_desc = {
    .name = "TurboScript.LineSlice.v1",
    .size = sizeof(ts_cflow_line_slice_t),
    .align = _Alignof(ts_cflow_line_slice_t),
    .kind = CMETA_T_OBJECT,
    .pointee = NULL,
    .traits = &ts_cflow_line_slice_traits,
    .identity = NULL
};

const cmeta_type_desc *ts_cflow_line_slice_type(void) {
  return &ts_cflow_line_slice_desc;
}

bool ts_cflow_text_lines_source_init(ts_cflow_text_lines_source_t *source,
                                     const char *text, size_t text_len) {
  size_t count = 1u;
  size_t line_index = 0u;
  size_t start = 0u;
  ts_cflow_line_slice_t *items;

  if (!source || (!text && text_len != 0u)) return false;
  if (!text) text = "";
  memset(source, 0, sizeof(*source));

  for (size_t i = 0u; i < text_len; ++i) {
    if (text[i] == '\n') ++count;
  }

  items = (ts_cflow_line_slice_t *)calloc(count, sizeof(*items));
  if (!items) return false;

  for (size_t i = 0u; i <= text_len; ++i) {
    if (i == text_len || text[i] == '\n') {
      size_t end = i;
      if (end > start && text && text[end - 1u] == '\r') --end;
      items[line_index].data = text ? text + start : NULL;
      items[line_index].len = end - start;
      ++line_index;
      start = i + 1u;
    }
  }

  source->items = items;
  source->count = line_index;
  return true;
}

bool ts_cflow_text_split_source_init(ts_cflow_text_lines_source_t *source,
                                     const char *text, size_t text_len,
                                     const char *separator,
                                     size_t separator_len) {
  ts_cflow_line_slice_t *items = NULL;
  size_t count = 0u;
  size_t cursor = 0u;
  size_t index = 0u;

  if (!source || (!text && text_len != 0u) ||
      (!separator && separator_len != 0u))
    return false;
  if (!text) text = "";
  if (!separator) separator = "";
  memset(source, 0, sizeof(*source));

  if (separator_len == 0u) {
    count = text_len;
    if (count != 0u) {
      items = (ts_cflow_line_slice_t *)calloc(count, sizeof(*items));
      if (!items) return false;
      for (size_t i = 0u; i < text_len; ++i) {
        items[i].data = text + i;
        items[i].len = 1u;
      }
    }
    source->items = items;
    source->count = count;
    return true;
  }

  count = 1u;
  cursor = 0u;
  while (cursor <= text_len) {
    size_t found = SIZE_MAX;
    for (size_t i = cursor; i + separator_len <= text_len; ++i) {
      if (memcmp(text + i, separator, separator_len) == 0) {
        found = i;
        break;
      }
    }
    if (found == SIZE_MAX) break;
    ++count;
    cursor = found + separator_len;
  }

  items = (ts_cflow_line_slice_t *)calloc(count, sizeof(*items));
  if (!items) return false;

  cursor = 0u;
  while (cursor <= text_len && index < count) {
    size_t found = SIZE_MAX;
    for (size_t i = cursor; i + separator_len <= text_len; ++i) {
      if (memcmp(text + i, separator, separator_len) == 0) {
        found = i;
        break;
      }
    }

    if (found == SIZE_MAX) {
      items[index].data = text ? text + cursor : NULL;
      items[index].len = text_len - cursor;
      ++index;
      break;
    }

    items[index].data = text ? text + cursor : NULL;
    items[index].len = found - cursor;
    ++index;
    cursor = found + separator_len;
  }

  source->items = items;
  source->count = index;
  return true;
}

void ts_cflow_text_lines_source_destroy(ts_cflow_text_lines_source_t *source) {
  if (!source) return;
  free(source->items);
  source->items = NULL;
  source->count = 0u;
}

bool ts_cflow_text_lines_plan_compile(cflow_graph *graph, cflow_plan *plan,
                                      const char **error) {
  if (error) *error = NULL;
  if (!graph || !plan) {
    if (error) *error = "text-line CFlow Plan requires graph and plan";
    return false;
  }

  memset(graph, 0, sizeof(*graph));
  memset(plan, 0, sizeof(*plan));
  graph->root = CMETA_INVALID_ID;
  cflow_graph_init(graph, &ts_cflow_line_slice_desc);
  if (graph->error) {
    if (error) *error = graph->error;
    cflow_graph_destroy(graph);
    graph->root = CMETA_INVALID_ID;
    return false;
  }

  if (!cflow_plan_compile_surface(plan, graph, NULL)) {
    if (error) *error = "text-line CFlow Plan compilation failed";
    cflow_plan_destroy(plan);
    cflow_graph_destroy(graph);
    graph->root = CMETA_INVALID_ID;
    return false;
  }
  return true;
}

bool ts_cflow_text_lines_plan_eval(const cflow_plan *plan,
                                   const ts_cflow_text_lines_source_t *source,
                                   cflow_result *out, const char **error) {
  if (error) *error = NULL;
  if (out) memset(out, 0, sizeof(*out));
  if (!plan || !source || !out ||
      (source->count != 0u && !source->items)) {
    if (error) *error = "text-line CFlow evaluation requires a valid source";
    return false;
  }

  if (!cflow_plan_eval_array(plan, source->items, source->count, out)) {
    if (error) *error = "text-line CFlow Plan execution failed";
    return false;
  }

  if (!cmeta_type_equal(out->type, &ts_cflow_line_slice_desc) ||
      out->count != source->count) {
    if (error) *error = "text-line CFlow Plan produced an invalid result";
    cflow_result_destroy(out);
    return false;
  }
  return true;
}

bool ts_cflow_text_lines_plan_count(const cflow_plan *plan,
                                    const ts_cflow_text_lines_source_t *source,
                                    size_t *out_count, const char **error) {
  cflow_result result = {0};

  if (out_count) *out_count = 0u;
  if (!out_count) {
    if (error) *error = "text-line CFlow count requires output storage";
    return false;
  }
  if (!ts_cflow_text_lines_plan_eval(plan, source, &result, error))
    return false;

  *out_count = result.count;
  cflow_result_destroy(&result);
  return true;
}