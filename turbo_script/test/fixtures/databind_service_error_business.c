#include "ErrorPlugin.StorePlugin.plugin.h"

int databind_11_ErrorPlugin_5_Store_4_Read(
    const Request_t *request,
    Response_t *response,
    databind_11_ErrorPlugin_5_Store_4_Read__error *error) {
  if (request == NULL || response == NULL || error == NULL) return -1;

  *error = (databind_11_ErrorPlugin_5_Store_4_Read__error)
      databind_11_ErrorPlugin_5_Store_4_Read__ERROR_INIT;

  if (request->id == 7u) {
    error->kind = databind_11_ErrorPlugin_5_Store_4_Read__ERROR_1;
    error->payload.error_1.id = request->id;
    return 0;
  }
  if (request->id == 99u) return -9;

  response->value = request->id * 10u;
  return 0;
}
