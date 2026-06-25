#ifndef OPENC6_FS_H
#define OPENC6_FS_H

#include <stdint.h>
#include <stddef.h>

#define SECTOR_SIZE 4096
#define FS_MAGIC    0xC6F5
#define TYPE_FILE   0x01
#define TYPE_DIR    0x02

#pragma pack(push, 1)

#pragma pack(push, 1)

typedef struct {
    uint16_t magic;
    uint16_t total_len;
    uint16_t data_len;
    uint16_t id;
    uint16_t parent_id;
    uint8_t  state;
    uint8_t  type;
    uint16_t chunk_idx;
    char     name[18];
} RecordHeader;

#pragma pack(pop)

#pragma pack(pop)

void fs_init(void);
void fs_format(void);

int16_t fs_mkdir(const char *name, uint16_t parent_id);
int16_t fs_write_file(const char *name, const uint8_t *data, uint32_t len, uint16_t parent_id);
int32_t fs_read_file(uint16_t file_id, uint8_t *dest, uint32_t offset, uint32_t len);
void fs_delete(uint16_t id);

int16_t fs_find_id(const char *name, uint16_t parent_id);
void fs_list_dir(uint16_t parent_id);
int32_t fs_get_size(uint16_t id);
uint8_t fs_get_type(uint16_t id);
int16_t fs_get_parent_id(uint16_t id);

#endif
