# OpenC6 File System (openc6_fs) - Technical Specification & Architecture

The **openc6_fs** is a lightweight, Power Loss Resilient, log-structured circular file system (LFS) designed specifically for resource-constrained microcontrollers (ESP32-C6). It provides dynamic wear leveling across flash memory without relying on a fixed File Allocation Table (FAT) or fixed superblock locations.

---

## 1. On-Flash Data Layout

The file system does not use a fixed sector hierarchy. All data (both directory metadata and file contents) is written sequentially as a continuous log. Each logical block begins with a `RecordHeader`, followed by the raw payload and padding bytes.

### Header Structure (RecordHeader)
The header occupies exactly 32 bytes and is tightly packed (`#pragma pack(push, 1)`):

```c
typedef struct {
    uint16_t magic;         // Record signature (always 0xC6F5)
    uint16_t total_len;     // Total record length (header + payload + padding)
    uint16_t data_len;      // Payload length in bytes
    uint16_t id;            // Unique object identifier
    uint16_t parent_id;     // Parent directory ID (0 is root)
    uint8_t  state;         // Record state: 1 (active), 0 (deleted/tombstone)
    uint8_t  type;          // Object type: 0x01 (file), 0x02 (directory)
    uint16_t chunk_idx;     // Chunk index for large split files (0, 1, 2...)
    char     name[18];      // Object name (up to 17 characters + null-terminator)
} RecordHeader;
```

### Special On-Flash Markers
* **`FS_MAGIC` (`0xC6F5`):** Indicates the presence of a valid file or directory record header.
* **`FS_MAGIC_SKIP` (`0xEEEE`):** Used to pad the remainder of a sector when the next chunk of a file cannot fit within the current sector boundaries. When scanning or parsing, the file system detects this marker and immediately skips to the start of the next physical sector.

---

## 2. Architectural Concepts and Algorithms

### Chunk-Based Segmented Allocation (Chunking)
To overcome the single-sector file size limit (4 KB), large files are automatically split into smaller chunks.
* The maximum payload size in a single chunk is **4064 bytes** (`SECTOR_SIZE (4096) - sizeof(RecordHeader) (32)`).
* If a chunk does not fit within the current sector, the remaining space is padded with an `FS_MAGIC_SKIP` record, and the next chunk starts clean at the beginning of the next sector. This ensures no record header or payload crosses a physical sector boundary.
* The total file size is calculated based on the highest recorded chunk index using the formula:
  $$\text{File Size} = (\text{chunk\_idx} \times 4064) + \text{data\_len\_of\_highest\_chunk}$$

### Dynamic RAM Indexing
To avoid scanning the physical flash during directory traversal or file lookups, the system maintains a flat in-memory index (`g_nodes`) in RAM.
* **Memory Optimization:** Upon startup, the system allocates **0 bytes** for the index. Memory is dynamically allocated and expanded on-demand using `realloc` in steps (16, 32, 64 nodes, etc.).
* **Heap Protection:** A hard safety limit `MAX_FILES_LIMIT` of **1024 objects** (files and directories combined) is enforced. This bounds RAM consumption to a maximum of ~28 KB even on a fully populated flash partition.
* **Flat Structure:** Each node in RAM contains only its metadata (`id`, `parent_id`, `type`, `name`, and `size`). No physical flash addresses are permanently stored in the RAM index.

### On-Demand Chunk Caching
For fast random-access file reads without scanning the entire log, an on-demand RAM caching mechanism is used:
* When `fs_read_file` is called, the system verifies if the cache is already built for the requested file. If not, it invalidates the current cache, analyzes the file size, and allocates a dynamic array of flash addresses (`needed_chunks * sizeof(uint32_t)`) using `malloc`.
* The system performs a single, fast linear scan from tail (`g_tail`) to head (`g_head`) to populate this cache array with physical flash addresses for each `chunk_idx`.
* Subsequent reads and seeks within this active file execute at `O(1)` speed.
* When writing, deleting, formatting, or switching files, the cache is freed (`free`), returning memory back to the system heap.

### Garbage Collection and Wear Leveling
The file system operates as a circular queue between `g_head` (where new data is appended) and `g_tail` (where the oldest data resides).
* If free space drops below `SECTOR_SIZE * 3`, garbage collection is triggered (`gc_step`).
* **Sector-by-Sector Evacuation:** The system reads the oldest sector at `g_tail`. For each record, if it is active and verified as the latest version via the on-demand cache, only that specific chunk is evacuated (appended) to `g_head`, updating its address in the cache.
* Outdated chunks (older versions of updated files or deleted files) are ignored. Once the sector is processed, it is erased with `flash_erase_sector`, and `g_tail` is advanced to the next sector.
* This ensures even, sequential wear across all sectors of the partition.

### Power Loss Resilience
Because the file system is log-structured and never overwrites files in-place (new versions of updated files are always written to `g_head` while old ones are simply marked as inactive or obsolete), a sudden power loss cannot corrupt existing files. Interrupted writes will naturally be ignored during the next startup scan because of incomplete headers or premature `0xFFFFFFFF` padding markers.

---

## 3. File System API Specification

* `void fs_init(void)` — Initializes the file system, locates `g_head` and `g_tail` by scanning boundary transitions, and builds the flat RAM index.
* `void fs_format(void)` — Erases all sectors in the partition, resets pointers, and frees all dynamically allocated RAM indexes and caches.
* `int16_t fs_mkdir(const char *name, uint16_t parent_id)` — Creates a directory record. Returns a unique directory ID or `-1` on error.
* `int16_t fs_write_file(const char *name, const uint8_t *data, uint32_t len, uint16_t parent_id)` — Writes a file, splitting it into chunks and appending them to `g_head`. Automatically invalidates older versions.
* `int32_t fs_read_file(uint16_t file_id, uint8_t *dest, uint32_t offset, uint32_t len)` — Reads file contents into a buffer, assembling fragmented chunks on the fly using the on-demand RAM cache.
* `void fs_delete(uint16_t id)` — Appends a "tombstone" record for the specified ID and removes it from the RAM index. Physical erasure happens during GC.
* `int16_t fs_find_id(const char *name, uint16_t parent_id)` — Searches the RAM index for an object by name within a given parent directory.
* `int16_t fs_get_parent_id(uint16_t id)` — Returns the parent directory ID of the specified object (used for relative `..` path parsing).
* `void fs_list_dir(uint16_t parent_id)` — Lists the contents of a directory to the console.
* `int32_t fs_get_size(uint16_t id)` — Returns the size of a file.
* `uint8_t fs_get_type(uint16_t id)` — Returns the type of an object (file or directory).

---

## 4. Physical and Practical Limits

* **Maximum Partition Size:** Theoretical limit is **4 GB** (due to `uint32_t` addressing). Practically recommended up to **16 MB** to maintain low startup mount times.
* **Maximum File Size:** Dynamic. There is no hardcoded chunk limit per file; it is bounded solely by the available heap space (RAM) required to allocate the chunk lookup cache array (`needed_chunks * sizeof(uint32_t)`) on demand.
* **Maximum Total Files:** **1024 objects** (files and directories combined, limited by RAM index `MAX_FILES_LIMIT`).
* **Maximum Filename Length:** **17 characters** (ASCII) + null-terminator.
