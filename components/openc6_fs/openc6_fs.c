#include "openc6_fs.h"
#include "hal_flash.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define MAX_FILES_LIMIT 1024
#define FS_MAGIC_SKIP 0xEEEE

static const char* TAG = "OPENC6_FS";

typedef struct {
    uint16_t id;
    uint16_t parent_id;
    uint8_t  type;
    char     name[18];
    uint32_t size;
} RamNode;

static RamNode *g_nodes = NULL;
static int g_node_capacity = 0;
static int g_node_count = 0;
static uint32_t g_head = 0;
static uint32_t g_tail = 0;
static uint16_t g_next_id = 1;

static uint16_t g_cached_file_id = 0xFFFF;
static uint32_t *g_chunk_cache = NULL;
static uint32_t g_cache_chunk_count = 0;

static void invalidate_cache(void) {
    g_cached_file_id = 0xFFFF;
    if (g_chunk_cache != NULL) {
        free(g_chunk_cache);
        g_chunk_cache = NULL;
    }
    g_cache_chunk_count = 0;
}

static int find_idx_by_id(uint16_t id) {
    for (int i = 0; i < g_node_count; i++) {
        if (g_nodes[i].id == id) return i;
    }
    return -1;
}

static void update_ram_index(uint16_t id, uint16_t parent_id, uint8_t type, const char *name, uint32_t size) {
    int idx = find_idx_by_id(id);
    if (idx >= 0) {
        g_nodes[idx].size = size;
    } else {
        if (g_node_count >= g_node_capacity) {
            int new_cap = (g_node_capacity == 0) ? 16 : g_node_capacity * 2;
            if (new_cap > MAX_FILES_LIMIT) {
                ESP_LOGE(TAG, "FS Index limit reached (%d files)!", MAX_FILES_LIMIT);
                return;
            }
            RamNode *new_nodes = realloc(g_nodes, new_cap * sizeof(RamNode));
            if (!new_nodes) {
                ESP_LOGE(TAG, "Out of RAM for FS index!");
                return;
            }
            g_nodes = new_nodes;
            g_node_capacity = new_cap;
        }
        g_nodes[g_node_count].id = id;
        g_nodes[g_node_count].parent_id = parent_id;
        g_nodes[g_node_count].type = type;
        strncpy(g_nodes[g_node_count].name, name, 17);
        g_nodes[g_node_count].name[17] = '\0';
        g_nodes[g_node_count].size = size;
        g_node_count++;
    }
    if (id >= g_next_id) g_next_id = id + 1;
}

static void remove_from_ram_index(uint16_t id) {
    int idx = find_idx_by_id(id);
    if (idx >= 0) {
        g_nodes[idx] = g_nodes[g_node_count - 1];
        g_node_count--;
    }
}

static uint32_t get_free_space(void) {
    if (g_head < g_tail) return g_tail - g_head;
    if (g_head > g_tail) return hal_flash_get_size() - g_head + g_tail;
    return hal_flash_get_size();
}

static void pad_sector_to_next(void) {
    uint32_t remain = SECTOR_SIZE - (g_head % SECTOR_SIZE);
    if (remain == SECTOR_SIZE) return;
    if (remain >= 4) {
        uint16_t pad_hdr[2] = {FS_MAGIC_SKIP, remain};
        flash_write(g_head, (uint8_t*)pad_hdr, 4);
    }
    g_head = (g_head + remain) % hal_flash_get_size();
}

static void build_cache(uint16_t id) {
    if (g_cached_file_id == id && g_chunk_cache != NULL) return;

    int idx = find_idx_by_id(id);
    if (idx < 0) return;

    invalidate_cache();

    uint32_t max_payload = SECTOR_SIZE - sizeof(RecordHeader);
    uint32_t file_size = g_nodes[idx].size;
    uint32_t needed_chunks = (file_size == 0) ? 1 : ((file_size + max_payload - 1) / max_payload);

    g_chunk_cache = (uint32_t*)malloc(needed_chunks * sizeof(uint32_t));
    if (!g_chunk_cache) {
        ESP_LOGE(TAG, "Failed to allocate cache for %lu chunks!", (unsigned long)needed_chunks);
        return;
    }

    g_cache_chunk_count = needed_chunks;
    g_cached_file_id = id;

    for (uint32_t i = 0; i < g_cache_chunk_count; i++) g_chunk_cache[i] = 0xFFFFFFFF;

    uint32_t flash_size = hal_flash_get_size();
    uint32_t curr = g_tail;

    while (curr != g_head) {
        uint16_t magic, tlen;
        flash_read(curr, (uint8_t*)&magic, 2);
        flash_read(curr + 2, (uint8_t*)&tlen, 2);

        if (magic == FS_MAGIC_SKIP) {
            curr = (curr + tlen) % flash_size;
            continue;
        } else if (magic == FS_MAGIC) {
            RecordHeader hdr;
            flash_read(curr, (uint8_t*)&hdr, sizeof(hdr));
            if (hdr.id == id && hdr.state == 1) {
                if (hdr.chunk_idx < g_cache_chunk_count) {
                    g_chunk_cache[hdr.chunk_idx] = curr;
                }
            } else if (hdr.id == id && hdr.state == 0) {
                for (uint32_t i = 0; i < g_cache_chunk_count; i++) g_chunk_cache[i] = 0xFFFFFFFF;
            }
            curr = (curr + tlen) % flash_size;
        } else {
            break;
        }
    }
}

static uint32_t append_chunk_raw(RecordHeader *hdr, const uint8_t *data) {
    uint32_t remain = SECTOR_SIZE - (g_head % SECTOR_SIZE);
    if (remain == SECTOR_SIZE) remain = SECTOR_SIZE;
    if (hdr->total_len > remain) pad_sector_to_next();

    uint32_t addr = g_head;
    flash_write(g_head, (uint8_t*)hdr, sizeof(RecordHeader));
    if (hdr->data_len > 0) flash_write(g_head + sizeof(RecordHeader), data, hdr->data_len);

    uint32_t pad_bytes = hdr->total_len - (sizeof(RecordHeader) + hdr->data_len);
    if (pad_bytes > 0) {
        uint32_t fff = 0xFFFFFFFF;
        flash_write(g_head + sizeof(RecordHeader) + hdr->data_len, (uint8_t*)&fff, pad_bytes);
    }
    g_head = (g_head + hdr->total_len) % hal_flash_get_size();
    return addr;
}

static void gc_step(void) {
    uint32_t sector_start = (g_tail / SECTOR_SIZE) * SECTOR_SIZE;
    uint32_t curr = sector_start;

    while (curr < sector_start + SECTOR_SIZE) {
        uint16_t magic, tlen;
        flash_read(curr, (uint8_t*)&magic, 2);
        flash_read(curr + 2, (uint8_t*)&tlen, 2);

        if (magic == FS_MAGIC_SKIP) {
            curr += tlen;
            continue;
        }
        if (magic != FS_MAGIC) break;

        RecordHeader hdr;
        flash_read(curr, (uint8_t*)&hdr, sizeof(hdr));

        if (hdr.state == 1 && find_idx_by_id(hdr.id) >= 0) {
            build_cache(hdr.id);
            if (g_chunk_cache && hdr.chunk_idx < g_cache_chunk_count && g_chunk_cache[hdr.chunk_idx] == curr) {
                uint8_t *buf = malloc(hdr.data_len);
                if (buf) {
                    flash_read(curr + sizeof(RecordHeader), buf, hdr.data_len);
                    uint32_t new_addr = append_chunk_raw(&hdr, buf);
                    g_chunk_cache[hdr.chunk_idx] = new_addr;
                    free(buf);
                }
            }
        }
        curr += tlen;
    }
    flash_erase_sector(sector_start / SECTOR_SIZE);
    g_tail = (sector_start + SECTOR_SIZE) % hal_flash_get_size();
}

static void check_gc(uint32_t needed_space) {
    while (get_free_space() < needed_space + SECTOR_SIZE * 3) {
        gc_step();
    }
}

void fs_format(void) {
    uint32_t num_sectors = hal_flash_get_size() / SECTOR_SIZE;
    for (uint32_t i = 0; i < num_sectors; i++) {
        flash_erase_sector(i);
    }
    g_head = 0;
    g_tail = 0;
    g_node_count = 0;
    g_next_id = 1;
    if (g_nodes) {
        free(g_nodes);
        g_nodes = NULL;
    }
    g_node_capacity = 0;
    invalidate_cache();
}

void fs_init(void) {
    if (g_nodes) {
        free(g_nodes);
        g_nodes = NULL;
    }
    g_node_capacity = 0;
    g_node_count = 0;
    g_head = 0;
    g_tail = 0;
    g_next_id = 1;
    invalidate_cache();

    uint32_t num_sectors = hal_flash_get_size() / SECTOR_SIZE;
    uint32_t gap_start = 0xFFFFFFFF;
    uint32_t gap_end = 0xFFFFFFFF;

    for (uint32_t i = 0; i < num_sectors; i++) {
        uint32_t current, next;
        flash_read(i * SECTOR_SIZE, (uint8_t*)&current, 4);
        flash_read(((i + 1) % num_sectors) * SECTOR_SIZE, (uint8_t*)&next, 4);

        if (current != 0xFFFFFFFF && next == 0xFFFFFFFF) gap_start = (i + 1) % num_sectors;
        if (current == 0xFFFFFFFF && next != 0xFFFFFFFF) gap_end = i;
    }

    if (gap_start == 0xFFFFFFFF) {
        fs_format();
        return;
    }

    g_tail = ((gap_end + 1) % num_sectors) * SECTOR_SIZE;
    uint32_t search_sec = (gap_start == 0) ? num_sectors - 1 : gap_start - 1;
    g_head = search_sec * SECTOR_SIZE;

    while (1) {
        uint16_t magic, tlen;
        flash_read(g_head, (uint8_t*)&magic, 2);
        if (magic == 0xFFFF) break;
        flash_read(g_head + 2, (uint8_t*)&tlen, 2);
        if (tlen == 0 || tlen > SECTOR_SIZE) break;
        g_head = (g_head + tlen) % hal_flash_get_size();
        if (g_head % SECTOR_SIZE == 0) break;
    }

    uint32_t curr = g_tail;
    while (curr != g_head) {
        uint16_t magic, tlen;
        flash_read(curr, (uint8_t*)&magic, 2);
        flash_read(curr + 2, (uint8_t*)&tlen, 2);

        if (magic == FS_MAGIC_SKIP) {
            curr = (curr + tlen) % hal_flash_get_size();
            continue;
        } else if (magic == FS_MAGIC) {
            RecordHeader hdr;
            flash_read(curr, (uint8_t*)&hdr, sizeof(hdr));
            if (hdr.state == 0) {
                remove_from_ram_index(hdr.id);
            } else {
                uint32_t max_payload = SECTOR_SIZE - sizeof(RecordHeader);
                uint32_t current_size = hdr.chunk_idx * max_payload + hdr.data_len;

                int idx = find_idx_by_id(hdr.id);
                if (idx >= 0) {
                    if (current_size > g_nodes[idx].size) g_nodes[idx].size = current_size;
                } else {
                    update_ram_index(hdr.id, hdr.parent_id, hdr.type, hdr.name, current_size);
                }
            }
            curr = (curr + tlen) % hal_flash_get_size();
        } else {
            break;
        }
    }
}

int16_t fs_find_id(const char *name, uint16_t parent_id) {
    for (int i = 0; i < g_node_count; i++) {
        if (g_nodes[i].parent_id == parent_id && strcmp(g_nodes[i].name, name) == 0) {
            return g_nodes[i].id;
        }
    }
    return -1;
}

int16_t fs_get_parent_id(uint16_t id) {
    if (id == 0) return 0;
    int idx = find_idx_by_id(id);
    return (idx >= 0) ? g_nodes[idx].parent_id : -1;
}

int16_t fs_mkdir(const char *name, uint16_t parent_id) {
    int16_t existing_id = fs_find_id(name, parent_id);
    if (existing_id >= 0) return existing_id;

    uint16_t id = g_next_id++;
    uint32_t padded_len = (sizeof(RecordHeader) + 3) & ~3;

    check_gc(padded_len);

    uint32_t remain = SECTOR_SIZE - (g_head % SECTOR_SIZE);
    if (padded_len > remain) pad_sector_to_next();

    RecordHeader hdr = {0};
    hdr.magic = FS_MAGIC;
    hdr.total_len = padded_len;
    hdr.data_len = 0;
    hdr.id = id;
    hdr.parent_id = parent_id;
    hdr.state = 1;
    hdr.type = TYPE_DIR;
    hdr.chunk_idx = 0;
    strncpy(hdr.name, name, 17);

    flash_write(g_head, (uint8_t*)&hdr, sizeof(hdr));
    g_head = (g_head + padded_len) % hal_flash_get_size();

    update_ram_index(id, parent_id, TYPE_DIR, name, 0);
    return id;
}

void fs_delete(uint16_t id) {
    int idx = find_idx_by_id(id);
    if (idx < 0) return;

    uint32_t padded_len = (sizeof(RecordHeader) + 3) & ~3;
    check_gc(padded_len);

    uint32_t remain = SECTOR_SIZE - (g_head % SECTOR_SIZE);
    if (padded_len > remain) pad_sector_to_next();

    RecordHeader hdr = {0};
    hdr.magic = FS_MAGIC;
    hdr.total_len = padded_len;
    hdr.data_len = 0;
    hdr.id = id;
    hdr.state = 0;

    flash_write(g_head, (uint8_t*)&hdr, sizeof(hdr));
    g_head = (g_head + padded_len) % hal_flash_get_size();

    remove_from_ram_index(id);
    if (g_cached_file_id == id) invalidate_cache();
}

int16_t fs_write_file(const char *name, const uint8_t *data, uint32_t len, uint16_t parent_id) {
    int16_t id = fs_find_id(name, parent_id);
    if (id >= 0) {
        fs_delete(id);
    } else {
        id = g_next_id++;
    }

    uint32_t offset = 0;
    uint16_t c_idx = 0;
    uint32_t max_payload = SECTOR_SIZE - sizeof(RecordHeader);

    while (offset < len || len == 0) {
        uint32_t chunk_len = len - offset;
        if (chunk_len > max_payload) chunk_len = max_payload;

        uint32_t padded_len = (sizeof(RecordHeader) + chunk_len + 3) & ~3;
        uint32_t remain = SECTOR_SIZE - (g_head % SECTOR_SIZE);

        if (padded_len > remain) {
            pad_sector_to_next();
            remain = SECTOR_SIZE;
        }

        check_gc(padded_len);

        RecordHeader hdr = {0};
        hdr.magic = FS_MAGIC;
        hdr.total_len = padded_len;
        hdr.data_len = chunk_len;
        hdr.id = id;
        hdr.parent_id = parent_id;
        hdr.state = 1;
        hdr.type = TYPE_FILE;
        hdr.chunk_idx = c_idx;
        strncpy(hdr.name, name, 17);

        flash_write(g_head, (uint8_t*)&hdr, sizeof(hdr));
        if (chunk_len > 0) flash_write(g_head + sizeof(hdr), data + offset, chunk_len);

        uint32_t pad_bytes = padded_len - (sizeof(RecordHeader) + chunk_len);
        if (pad_bytes > 0) {
            uint32_t fff = 0xFFFFFFFF;
            flash_write(g_head + sizeof(hdr) + chunk_len, (uint8_t*)&fff, pad_bytes);
        }

        g_head = (g_head + padded_len) % hal_flash_get_size();

        offset += chunk_len;
        c_idx++;
        if (len == 0) break;
    }

    update_ram_index(id, parent_id, TYPE_FILE, name, len);
    if (g_cached_file_id == id) invalidate_cache();
    return id;
}

int32_t fs_read_file(uint16_t id, uint8_t *dest, uint32_t offset, uint32_t len) {
    int idx = find_idx_by_id(id);
    if (idx < 0 || g_nodes[idx].type != TYPE_FILE) return -1;

    uint32_t file_size = g_nodes[idx].size;
    if (offset >= file_size) return 0;
    if (offset + len > file_size) len = file_size - offset;

    build_cache(id);
    if (!g_chunk_cache) return -1;

    uint32_t max_payload = SECTOR_SIZE - sizeof(RecordHeader);
    uint32_t start_chunk = offset / max_payload;
    uint32_t end_chunk = (offset + len - 1) / max_payload;
    uint32_t read_count = 0;

    for (uint32_t c = start_chunk; c <= end_chunk; c++) {
        if (c >= g_cache_chunk_count) break;
        uint32_t addr = g_chunk_cache[c];
        if (addr == 0xFFFFFFFF) continue;

        RecordHeader hdr;
        flash_read(addr, (uint8_t*)&hdr, sizeof(hdr));

        uint32_t chunk_offset = (c == start_chunk) ? (offset % max_payload) : 0;
        uint32_t bytes_to_read = hdr.data_len - chunk_offset;
        if (read_count + bytes_to_read > len) bytes_to_read = len - read_count;

        flash_read(addr + sizeof(RecordHeader) + chunk_offset, dest + read_count, bytes_to_read);
        read_count += bytes_to_read;
    }
    return read_count;
}

void fs_list_dir(uint16_t parent_id) {
    printf("Listing directory (ID: %d):\n", parent_id);
    printf("%-5s %-4s %-16s %s\n", "ID", "TYPE", "NAME", "SIZE");
    printf("----------------------------------------\n");
    for (int i = 0; i < g_node_count; i++) {
        if (g_nodes[i].parent_id == parent_id) {
            printf("%-5d %-4s %-16s %lu\n",
                   g_nodes[i].id,
                   g_nodes[i].type == TYPE_DIR ? "DIR" : "FILE",
                   g_nodes[i].name,
                   (unsigned long)g_nodes[i].size);
        }
    }
}

int32_t fs_get_size(uint16_t id) {
    int idx = find_idx_by_id(id);
    return (idx >= 0) ? (int32_t)g_nodes[idx].size : -1;
}

uint8_t fs_get_type(uint16_t id) {
    int idx = find_idx_by_id(id);
    return (idx >= 0) ? g_nodes[idx].type : 0;
}
