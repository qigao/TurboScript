#include "Image.ImageProcessor.plugin.h"

int databind_5_Image_5_Codec_6_Decode(
    const DecodeRequest_t *request,
    DecodeResponse_t *response) {
  if (request == NULL || response == NULL) return -1;
  response->pixels = request->width * request->scale;
  return 0;
}
