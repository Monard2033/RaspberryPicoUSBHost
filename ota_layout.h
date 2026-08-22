#pragma once

#include <stddef.h>
#include <stdint.h>

#define OTA_FLASH_TOTAL_SIZE       (4u * 1024u * 1024u)
#define OTA_BOOTLOADER_OFFSET      0x00000000u
#define OTA_BOOTLOADER_SIZE        (64u * 1024u)
#define OTA_ACTIVE_OFFSET          OTA_BOOTLOADER_SIZE
#define OTA_STAGING_OFFSET         (2u * 1024u * 1024u)
#define OTA_METADATA_OFFSET        (OTA_FLASH_TOTAL_SIZE - 4096u)
#define OTA_ACTIVE_MAX_SIZE        (OTA_STAGING_OFFSET - OTA_ACTIVE_OFFSET)
#define OTA_STAGING_MAX_SIZE       OTA_ACTIVE_MAX_SIZE
#define OTA_APP_VECTOR_OFFSET      0x100u

#define OTA_PROTOCOL_VERSION       0x01u
#define OTA_TARGET_RP2040          0x01u
#define OTA_BOARD_WEACT_RP2040_4MB 0x2040u

#define OTA_METADATA_MAGIC         0x574B4F54u /* "WKOT" */
#define OTA_METADATA_FORMAT        0x0001u
#define OTA_METADATA_STATE_PENDING 0xFFFFFFFEu
#define OTA_METADATA_STATE_INSTALLED 0xFFFFFFFCu

struct ota_install_metadata {
    uint32_t magic;
    uint16_t format;
    uint8_t target;
    uint8_t protocol;
    uint16_t board_id;
    uint8_t session;
    uint8_t command_token;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t immutable_crc32;
    uint32_t state;
    uint8_t reserved[228];
} __attribute__((packed));

_Static_assert(sizeof(struct ota_install_metadata) == 256u,
               "OTA metadata must occupy exactly one flash page");
_Static_assert(OTA_ACTIVE_OFFSET + OTA_ACTIVE_MAX_SIZE == OTA_STAGING_OFFSET,
               "Active image must end at the staging boundary");
_Static_assert(OTA_STAGING_OFFSET + OTA_STAGING_MAX_SIZE <= OTA_METADATA_OFFSET,
               "Staging image must not overlap OTA metadata");

static inline uint32_t ota_crc32_update(uint32_t crc,
                                        const uint8_t *data,
                                        size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8u; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u &
                                (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return crc;
}

static inline uint32_t ota_crc32(const uint8_t *data, size_t length)
{
    return ~ota_crc32_update(0xFFFFFFFFu, data, length);
}

static inline uint32_t ota_metadata_immutable_crc(
    const struct ota_install_metadata *metadata)
{
    return ota_crc32((const uint8_t *)metadata,
                     offsetof(struct ota_install_metadata,
                              immutable_crc32));
}

static inline int ota_metadata_is_valid(
    const struct ota_install_metadata *metadata)
{
    return metadata->magic == OTA_METADATA_MAGIC &&
           metadata->format == OTA_METADATA_FORMAT &&
           metadata->target == OTA_TARGET_RP2040 &&
           metadata->protocol == OTA_PROTOCOL_VERSION &&
           metadata->board_id == OTA_BOARD_WEACT_RP2040_4MB &&
           metadata->image_size > OTA_APP_VECTOR_OFFSET + 8u &&
           metadata->image_size <= OTA_ACTIVE_MAX_SIZE &&
           metadata->immutable_crc32 ==
               ota_metadata_immutable_crc(metadata);
}
