#ifndef TURBOSCRIPT_FIN_PLUGIN_FUNCTIONS_H
#define TURBOSCRIPT_FIN_PLUGIN_FUNCTIONS_H

#include <cmeta/function.h>

FunctionDecl(value, double, ts_fin_kelly,
    (double, win_rate, CMETA_PARAM_IN),
    (double, avg_win, CMETA_PARAM_IN),
    (double, avg_loss, CMETA_PARAM_IN));

#endif /* TURBOSCRIPT_FIN_PLUGIN_FUNCTIONS_H */
