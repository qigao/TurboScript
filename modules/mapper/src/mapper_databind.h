#ifndef TURBOSCRIPT_MAPPER_DATABIND_H
#define TURBOSCRIPT_MAPPER_DATABIND_H

#include "mapper.h"
#include "exprtk_class.h"

#include <data_bind.h>

#include <stddef.h>

int mapper_databind_decode(
    mapper_ctx_t *ctx, exprtk_class_t *klass, DataBindFormat format,
    const char *data, size_t len, exprtk_value_t *out,
    char *error, size_t error_len);

int mapper_databind_encode(
    mapper_ctx_t *ctx, exprtk_instance_t *instance, DataBindFormat format,
    char **out_data, size_t *out_len, char *error, size_t error_len);

void mapper_databind_clear(mapper_ctx_t *ctx);

#endif
