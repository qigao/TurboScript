#include "tinytest.h"
#include "exprtk.h"
#include "turbo_script_cmeta_bridge.h"

#include <cmeta/cmeta.h>

static exprtk_node_t *ts_test_single_expr(const char *source,
                                          exprtk_node_t **root_out) {
  exprtk_node_t *root = exprtk_parse(source, 0);
  if (root_out) *root_out = root;
  if (!root || root->type != EXPRTK_NODE_BLOCK ||
      root->data.block.count != 1u)
    return NULL;
  return root->data.block.statements[0];
}

describe(cmeta_bridge) {
  it("maps TurboScript scalar and vector flow types") {
    ts_cmeta_value_semantics_t semantics;

    check_true(ts_cmeta_describe_value_type(EXPRTK_VAL_NUMBER, &semantics));
    check((semantics.shape) == (TS_CMETA_VALUE_SCALAR));
    check_true(cmeta_type_equal(semantics.flow_type, &cmeta_type_double));

    check_true(ts_cmeta_describe_value_type(EXPRTK_VAL_BOOL, &semantics));
    check((semantics.shape) == (TS_CMETA_VALUE_SCALAR));
    check_true(cmeta_type_equal(semantics.flow_type, &cmeta_type_bool));

    check_true(ts_cmeta_describe_value_type(EXPRTK_VAL_VECTOR, &semantics));
    check((semantics.shape) == (TS_CMETA_VALUE_VECTOR));
    check_true(cmeta_type_equal(semantics.flow_type, &cmeta_type_double));

    check_false(ts_cmeta_describe_value_type(EXPRTK_VAL_MAP, &semantics));
  }

  it("describes a pure numeric map lambda") {
    exprtk_node_t *root = NULL;
    exprtk_node_t *lambda = ts_test_single_expr("x => x * 2", &root);
    ts_cmeta_lambda_contract_t contract;
    const cmeta_sig_desc *signature;
    const char *error = NULL;

    check_not_null(lambda);
    check_true(ts_cmeta_analyze_lambda(lambda, TS_CMETA_LAMBDA_MAP,
                                       &contract, &error));
    check_null(error);
    check((contract.arity) == (1u));
    check((contract.capture_count) == (0u));
    check_false(contract.executable);
    check_true(cmeta_effects_are_pure(contract.effects));
    check_true(cmeta_properties_include(contract.properties,
                                        CMETA_PROP_DETERMINISTIC));
    check_true(cmeta_callable_contract_valid(contract.callable));

    signature = cmeta_callable_signature(contract.callable);
    check_not_null(signature);
    check_true(cmeta_type_equal(signature->params[0], &cmeta_type_double));
    check_true(cmeta_type_equal(signature->return_type, &cmeta_type_double));

    exprtk_free(root);
  }

  it("describes a pure numeric filter lambda") {
    exprtk_node_t *root = NULL;
    exprtk_node_t *lambda = ts_test_single_expr("x => x > 0", &root);
    ts_cmeta_lambda_contract_t contract;
    const cmeta_sig_desc *signature;
    const char *error = NULL;

    check_not_null(lambda);
    check_true(ts_cmeta_analyze_lambda(lambda, TS_CMETA_LAMBDA_FILTER,
                                       &contract, &error));
    check_true(cmeta_effects_are_pure(contract.effects));
    check_true(cmeta_callable_contract_valid(contract.callable));

    signature = cmeta_callable_signature(contract.callable);
    check_not_null(signature);
    check_true(cmeta_type_equal(signature->params[0], &cmeta_type_double));
    check_true(cmeta_type_equal(signature->return_type, &cmeta_type_bool));

    exprtk_free(root);
  }

  it("describes a pure numeric reducer lambda") {
    exprtk_node_t *root = NULL;
    exprtk_node_t *lambda = ts_test_single_expr("(acc, x) => acc + x", &root);
    ts_cmeta_lambda_contract_t contract;
    const cmeta_sig_desc *signature;
    const char *error = NULL;

    check_not_null(lambda);
    check_true(ts_cmeta_analyze_lambda(lambda, TS_CMETA_LAMBDA_REDUCE,
                                       &contract, &error));
    check((contract.arity) == (2u));
    check_true(cmeta_effects_are_pure(contract.effects));

    signature = cmeta_callable_signature(contract.callable);
    check_not_null(signature);
    check((signature->param_count) == (2u));
    check_true(cmeta_type_equal(signature->params[0], &cmeta_type_double));
    check_true(cmeta_type_equal(signature->params[1], &cmeta_type_double));
    check_true(cmeta_type_equal(signature->return_type, &cmeta_type_double));

    exprtk_free(root);
  }

  it("marks captured variables conservatively unknown") {
    exprtk_node_t *root = NULL;
    exprtk_node_t *lambda = ts_test_single_expr("x => x * factor", &root);
    ts_cmeta_lambda_contract_t contract;
    const char *error = NULL;

    check_not_null(lambda);
    check_true(ts_cmeta_analyze_lambda(lambda, TS_CMETA_LAMBDA_MAP,
                                       &contract, &error));
    check((contract.capture_count) == (1u));
    check_false(cmeta_effects_are_pure(contract.effects));
    check_true((contract.effects & CMETA_EFFECT_UNKNOWN) != 0u);
    check((contract.properties) == (CMETA_PROP_NONE));

    exprtk_free(root);
  }

  it("rejects lambda arity that does not match the CFlow operator") {
    exprtk_node_t *root = NULL;
    exprtk_node_t *lambda = ts_test_single_expr("(a, b) => a + b", &root);
    ts_cmeta_lambda_contract_t contract;
    const char *error = NULL;

    check_not_null(lambda);
    check_false(ts_cmeta_analyze_lambda(lambda, TS_CMETA_LAMBDA_MAP,
                                        &contract, &error));
    check_not_null(error);

    exprtk_free(root);
  }
}
