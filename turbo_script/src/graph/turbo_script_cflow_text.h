#ifndef TURBO_SCRIPT_CFLOW_TEXT_H
#define TURBO_SCRIPT_CFLOW_TEXT_H

#include <cflow/graph.h>
#include <cflow/plan.h>
#include <cmeta/cmeta.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Borrowed line slice used by the first text/file CFlow source seam.
 *
 * The slice owns no characters. Its data pointer is valid only while the
 * originating TurboScript string value remains alive. The CMeta descriptor is
 * therefore TRIVIAL_COPY/TRIVIAL_DESTROY: CFlow may copy slice descriptors,
 * but never the pointed-to text.
 */
typedef struct ts_cflow_line_slice {
  const char *data;
  size_t len;
} ts_cflow_line_slice_t;

typedef struct ts_cflow_text_lines_source {
  ts_cflow_line_slice_t *items;
  size_t count;
} ts_cflow_text_lines_source_t;

const cmeta_type_desc *ts_cflow_line_slice_type(void);

/*
 * Split text with the same eager semantics as the legacy stream.text(...).
 * lines() adapter:
 * - '\n' terminates a line;
 * - a trailing '\r' before a terminator/end is omitted;
 * - empty input contains one empty line;
 * - trailing '\n' therefore produces a final empty line.
 *
 * The returned source owns only the slice array and borrows text.
 */
bool ts_cflow_text_lines_source_init(ts_cflow_text_lines_source_t *source,
                                     const char *text, size_t text_len);

/*
 * Split text with the same eager byte-oriented semantics as legacy
 * stream.text(...).split(separator):
 * - empty separator yields one-byte slices and zero slices for empty text;
 * - non-empty separator preserves leading, trailing, and adjacent empties;
 * - separator matches are non-overlapping and advance by separator length.
 */
bool ts_cflow_text_split_source_init(ts_cflow_text_lines_source_t *source,
                                     const char *text, size_t text_len,
                                     const char *separator,
                                     size_t separator_len);

void ts_cflow_text_lines_source_destroy(ts_cflow_text_lines_source_t *source);

/*
 * Compile the identity typed CFlow Plan for line values. The graph is kept
 * separately so tests/diagnostics can inspect the typed topology.
 */
bool ts_cflow_text_lines_plan_compile(cflow_graph *graph, cflow_plan *plan,
                                      const char **error);

/*
 * Evaluate one line source through the compiled Plan.
 * The returned result owns only copied line-slice descriptors; their data
 * pointers still borrow the originating text and must be consumed before that
 * TurboScript string value is released.
 */
bool ts_cflow_text_lines_plan_eval(const cflow_plan *plan,
                                   const ts_cflow_text_lines_source_t *source,
                                   cflow_result *out, const char **error);

/* Convenience scalar terminal over the same Plan/result path. */
bool ts_cflow_text_lines_plan_count(const cflow_plan *plan,
                                    const ts_cflow_text_lines_source_t *source,
                                    size_t *out_count, const char **error);

#ifdef __cplusplus
}
#endif

#endif /* TURBO_SCRIPT_CFLOW_TEXT_H */