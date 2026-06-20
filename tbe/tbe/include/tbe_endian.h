#ifndef TURBONET_SBE_ENDIAN_H
#define TURBONET_SBE_ENDIAN_H

#include <stdint.h>

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
  #define TBE_IS_LITTLE_ENDIAN (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#elif defined(_WIN32) || defined(_MSC_VER)
  #define TBE_IS_LITTLE_ENDIAN 1
#elif defined(__LITTLE_ENDIAN__)
  #define TBE_IS_LITTLE_ENDIAN 1
#elif defined(__BIG_ENDIAN__)
  #define TBE_IS_LITTLE_ENDIAN 0
#else
  #define TBE_IS_LITTLE_ENDIAN 1
#endif

#if defined(_MSC_VER) && !defined(__clang__)
  #include <intrin.h>
  #define TBE_BSWAP16(x) _byteswap_ushort(x)
  #define TBE_BSWAP32(x) _byteswap_ulong(x)
  #define TBE_BSWAP64(x) _byteswap_uint64(x)
#else
  #define TBE_BSWAP16(x) __builtin_bswap16(x)
  #define TBE_BSWAP32(x) __builtin_bswap32(x)
  #define TBE_BSWAP64(x) __builtin_bswap64(x)
#endif

#if TBE_IS_LITTLE_ENDIAN
  #define tbe_le16toh(x) (uint16_t)(x)
  #define tbe_le32toh(x) (uint32_t)(x)
  #define tbe_le64toh(x) (uint64_t)(x)
  #define tbe_htole16(x) (uint16_t)(x)
  #define tbe_htole32(x) (uint32_t)(x)
  #define tbe_htole64(x) (uint64_t)(x)
#else
  #define tbe_le16toh(x) TBE_BSWAP16((uint16_t)(x))
  #define tbe_le32toh(x) TBE_BSWAP32((uint32_t)(x))
  #define tbe_le64toh(x) TBE_BSWAP64((uint64_t)(x))
  #define tbe_htole16(x) TBE_BSWAP16((uint16_t)(x))
  #define tbe_htole32(x) TBE_BSWAP32((uint32_t)(x))
  #define tbe_htole64(x) TBE_BSWAP64((uint64_t)(x))
#endif

#if TBE_IS_LITTLE_ENDIAN
  #define tbe_be16toh(x) TBE_BSWAP16((uint16_t)(x))
  #define tbe_be32toh(x) TBE_BSWAP32((uint32_t)(x))
  #define tbe_be64toh(x) TBE_BSWAP64((uint64_t)(x))
  #define tbe_htobe16(x) TBE_BSWAP16((uint16_t)(x))
  #define tbe_htobe32(x) TBE_BSWAP32((uint32_t)(x))
  #define tbe_htobe64(x) TBE_BSWAP64((uint64_t)(x))
#else
  #define tbe_be16toh(x) (uint16_t)(x)
  #define tbe_be32toh(x) (uint32_t)(x)
  #define tbe_be64toh(x) (uint64_t)(x)
  #define tbe_htobe16(x) (uint16_t)(x)
  #define tbe_htobe32(x) (uint32_t)(x)
  #define tbe_htobe64(x) (uint64_t)(x)
#endif

#endif /* TURBONET_SBE_ENDIAN_H */
