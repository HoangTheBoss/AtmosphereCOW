/*
 * Copyright (c) 2026 HOS_COW Project
 *
 * GPLv2+ — Sparse Block-Level Copy-on-Write (COW) Engine for cowMMC (emummc.kip)
 * Designed to compile both in freestanding ARM64 (FS.kip / emummc.kip) and
 * on host (macOS/Linux) for deterministic unit and crash-consistency fuzz testing.
 */

#ifndef __COW_ENGINE_H__
#define __COW_ENGINE_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Geometry Constants */
#define COW_SECTOR_SIZE          512U
#define COW_SECTOR_SHIFT         9U
#define COW_BLOCK_SECTORS        32U                            /* 16 KB per COW block */
#define COW_BLOCK_SHIFT          5U                             /* 2^5 = 32 sectors */
#define COW_BLOCK_MASK           (COW_BLOCK_SECTORS - 1U)       /* 0x1F */
#define COW_BLOCK_BYTES          (COW_BLOCK_SECTORS * COW_SECTOR_SIZE) /* 16384 bytes */

/* 2-Level Lookup Table Geometry (1024..8192 L1 entries -> 64..512 GiB at 16 KB granularity) */
#define COW_MIN_L1_ENTRIES        1024U                          /* 1024 * 64 MB = 64 GiB minimum ceiling */
#define COW_L1_ENTRIES            8192U                          /* Max supported L1 entries (512 GiB) */
#define COW_L1_SECTORS_FOR(n)     (((uint32_t)(n) * (uint32_t)sizeof(uint32_t)) / COW_SECTOR_SIZE)
#define COW_L1_SLOTS_FOR(n)       (((((uint32_t)(n) * (uint32_t)sizeof(uint32_t)) + COW_BLOCK_BYTES - 1U)) / COW_BLOCK_BYTES)
#define COW_V2_DEFAULT_L1_ENTRIES 4096U                          /* 256 GiB default virtual ceiling (1 slot/copy) */
#define COW_L2_SHIFT              12U                            /* 2^12 = 4096 entries per L2 page */
#define COW_L2_ENTRIES            (1U << COW_L2_SHIFT)           /* 4096 * 4 B = 16 KB (1 slot) */
#define COW_L2_MASK               (COW_L2_ENTRIES - 1U)          /* 0xFFF */
#define COW_L2_ENTRIES_PER_SECT   (COW_SECTOR_SIZE / sizeof(uint32_t)) /* 128 entries per 512B sector */

/* In-RAM LRU Cache of L2 Pages inside emummc.kip .bss (8 * 16 KB = 128 KB) */
#ifndef COW_L2_CACHE_WAYS
#define COW_L2_CACHE_WAYS         8U
#endif

/* Reserved Superblock Slots at start of cowMMC pool region */
#define COW_SUPERBLOCK_SLOTS      2U                             /* Slot 0 (Ping) and Slot 1 (Pong) */
#define COW_FIRST_DATA_SLOT       (COW_SUPERBLOCK_SLOTS + 2U * COW_L1_SLOTS_FOR(COW_MIN_L1_ENTRIES))

/* Default 32 MB prefix stored in /emuMMC/COWx/eMMC/00 for bootloader compatibility */
#define COW_DEFAULT_PREFIX_SECTS  65536U                         /* 32 MB = 65536 sectors */

/* Magic, Version (v2-only), Allocation Policies & Flags */
#define COW_MAGIC                 0x30574F43U                    /* "COW0" in little-endian */
#define COW_VERSION_V2            2U
#define COW_VERSION               COW_VERSION_V2                 /* Only v2 extended superblock format supported */

#define COW_FLAG_EPHEMERAL       (1U << 0)                      /* Auto-reset overlay on every boot */
#define COW_FLAG_THIN_PROVISION  (1U << 1)                      /* Pre-boot thin pool auto-grow enabled */
#define COW_FLAG_NEEDS_REBASE    (1U << 2)                      /* Physical sysNAND changed compatibly; rebase advised */
#define COW_FLAG_FW_MISMATCH     (1U << 3)                      /* Physical BOOT0/BOOT1/GPT/FW changed; mount blocked */
#define COW_FLAG_CONFLICT_LOCKED (1U << 4)                      /* Unresolved write-write conflicts present */

#define COW_ALLOC_THICK          0U
#define COW_ALLOC_THIN           1U

/* Drift Mask Bits (base_drift_part_mask) */
#define COW_DRIFT_BOOT0_PKG1     (1U << 0)
#define COW_DRIFT_BOOT0_PATROL   (1U << 1)
#define COW_DRIFT_BOOT1          (1U << 2)
#define COW_DRIFT_GPT            (1U << 3)
#define COW_DRIFT_BCPKG2         (1U << 4)
#define COW_DRIFT_PRODINFO       (1U << 5)
#define COW_DRIFT_SAFE           (1U << 6)
#define COW_DRIFT_SYSTEM         (1U << 7)
#define COW_DRIFT_USER           (1U << 8)

/* On-Disk Superblock Header (512 bytes, stored at sector 0 of Slot 0 and Slot 1).
 * Bytes 0x034..0x1FB hold extended geometry, base-generation fingerprints,
 * and drift/reconciliation metadata, while ping-pong L1 copies reside at
 * `l1_slot_a` and `l1_slot_b`. Legacy v1 superblocks (version == 1) are rejected.
 */
typedef struct __attribute__((packed)) {
    /* 0x000..0x033 (52 bytes): Primary header */
    uint32_t magic;                      /* 0x000: COW_MAGIC ("COW0") */
    uint32_t version;                    /* 0x004: COW_VERSION_V2 (2) */
    uint64_t seq_num;                    /* 0x008: Monotonically increasing commit sequence number */
    uint64_t gpp_total_sectors;          /* 0x010: Physical GPP sectors exposed to HOS (must equal physical_base_sectors) */
    uint32_t prefix_sectors;             /* 0x018: Sectors routed to /eMMC/00 (e.g., 65536 = 32 MiB) */
    uint32_t total_slots;                /* 0x01C: Currently backed 16 KiB slots in /eMMC/00..NN */
    uint32_t next_free_slot;             /* 0x020: Next unallocated 16 KiB slot index */
    uint32_t flags;                      /* 0x024: COW_FLAG_* bitmask */
    uint32_t dirty_data_blocks;          /* 0x028: Number of allocated 16 KiB data blocks */
    uint32_t dirty_l2_pages;             /* 0x02C: Number of allocated 16 KiB L2 table pages */
    uint32_t l1_crc32;                   /* 0x030: IEEE 802.3 CRC32 of active L1 table (l1_entries * 4 B) */

    /* 0x034..0x1FB (456 bytes): Extended Geometry, Capacity & Generation Metadata */
    uint32_t header_ext_size;            /* 0x034: 512 */
    uint64_t physical_base_sectors;      /* 0x038: Underlying physical eMMC GPP sectors (~122M OLED) */
    uint32_t max_overlay_slots;          /* 0x040: Policy maximum COW pool slots (thin ceiling / thick size) */
    uint32_t l1_entries;                 /* 0x044: Active L1 entries (1024..8192) */
    uint32_t l1_slot_a;                  /* 0x048: Starting 16 KiB slot for SB0's L1 table */
    uint32_t l1_slot_b;                  /* 0x04C: Starting 16 KiB slot for SB1's L1 table */
    uint32_t first_data_slot;            /* 0x050: First pool slot available for L2/data */
    uint32_t thin_grow_step_slots;       /* 0x054: Pre-boot thin growth step in 16 KiB slots */
    uint16_t thin_high_watermark_pct;    /* 0x058: Pre-boot grow trigger usage threshold (e.g., 80) */
    uint8_t  alloc_policy;               /* 0x05A: COW_ALLOC_THICK (0) or COW_ALLOC_THIN (1) */
    uint8_t  block_shift;                /* 0x05B: 5 (32 sectors = 16 KiB) */
    uint8_t  l2_shift;                   /* 0x05C: 12 (4096 entries per L2 page) */
    uint8_t  reserved_geom[3];           /* 0x05D: Zero-padded alignment */
    uint32_t part_max_sectors;           /* 0x060: FAT32 split part size in sectors (e.g., 8380416 = 4092 MiB) */
    uint32_t base_hos_version;           /* 0x064: Packed HOS/pkg1/mkey identifier */
    uint64_t base_gen_id;                /* 0x068: Physical sysNAND generation identifier */
    uint8_t  overlay_uuid[16];           /* 0x070: Unique overlay UUID */
    uint8_t  parent_snapshot_uuid[16];   /* 0x080: Pre-rebase snapshot / parent generation UUID */
    uint32_t base_pkg1_ver;              /* 0x090: Package1 build/key_generation marker */
    uint32_t ext_meta_slot;              /* 0x094: Reserved slot for future extended metadata */
    uint8_t  emmc_cid_sha256_16[16];     /* 0x098: Truncated SHA-256 of physical eMMC CID */
    uint8_t  boot0_pkg1_sha256[32];      /* 0x0A8: SHA-256 of BOOT0 BCT+Pkg1 (excl. BctSave & NAND Patrol) */
    uint8_t  boot1_sha256[32];           /* 0x0C8: SHA-256 of BOOT1 (4 MiB) */
    uint8_t  gpt_sha256[32];             /* 0x0E8: SHA-256 of physical GPP LBA 0..33 */
    uint8_t  bcpkg2_sha256[32];          /* 0x108: SHA-256 of BCPKG2-1-Normal-Main (8 MiB) */
    uint8_t  system_digest_sha256[32];   /* 0x128: Digest of SYSTEM partition / superblock anchor */
    uint8_t  user_digest_sha256[32];     /* 0x148: Digest of USER partition / superblock anchor */
    char     overlay_name[32];           /* 0x168: NUL-terminated overlay label (e.g., "COW0") */
    uint64_t created_epoch;              /* 0x188: Creation epoch/counter */
    uint64_t last_rebase_seq;            /* 0x190: seq_num at last clean rebase */
    uint64_t last_verified_base_gen_id;  /* 0x198: Last verified physical base generation ID */
    uint32_t snapshot_count;             /* 0x1A0: Retained recovery snapshot count */
    uint32_t conflict_count;             /* 0x1A4: Dirty block conflict count from last verification */
    uint32_t base_drift_part_mask;       /* 0x1A8: Bitmask of drifted regions (COW_DRIFT_*) */
    uint32_t reserved_meta;              /* 0x1AC: Zero */
    uint8_t  boot0_patrol_sha256_16[16]; /* 0x1B0: SHA-256[0:16] of BOOT0[0x184000..0x184200] */
    uint8_t  last_snapshot_sha256_16[16];/* 0x1C0: SHA-256[0:16] of last pre-rebase snapshot */
    uint8_t  reserved_v2[44];            /* 0x1D0: Zero-padded reserved bytes */

    /* 0x1FC..0x1FF (4 bytes) */
    uint32_t header_crc32;               /* 0x1FC: IEEE 802.3 CRC32 of bytes 0x000..0x1FB */
} cow_header_t;

_Static_assert(sizeof(cow_header_t) == COW_SECTOR_SIZE, "cow_header_t must be exactly 512 bytes");
_Static_assert(offsetof(cow_header_t, header_ext_size) == 0x034, "cow_header_t ext offset");
_Static_assert(offsetof(cow_header_t, physical_base_sectors) == 0x038, "cow_header_t phys offset");
_Static_assert(offsetof(cow_header_t, header_crc32) == 0x1FC, "cow_header_t crc offset");

/* Platform I/O Callbacks (Return true on success, false on I/O failure) */
typedef struct {
    void *user_ctx;

    /* Read sectors from physical sysMMC GPP (READ-ONLY — no write callback exists by design!) */
    bool (*read_base_gpp)(void *ctx, uint32_t sector, uint32_t num_sectors, void *buf);

    /* Read/write sectors in the 32 MB prefix region (/emuMMC/COW0/eMMC/00 sectors 0..prefix_sectors-1) */
    bool (*read_prefix)(void *ctx, uint32_t sector, uint32_t num_sectors, void *buf);
    bool (*write_prefix)(void *ctx, uint32_t sector, uint32_t num_sectors, const void *buf);

    /* Read/write sectors in the pre-allocated sparse COW pool (/emuMMC/COW0/eMMC/00..NN) */
    bool (*read_cow)(void *ctx, uint32_t cow_sector, uint32_t num_sectors, void *buf);
    bool (*write_cow)(void *ctx, uint32_t cow_sector, uint32_t num_sectors, const void *buf);
    bool (*sync_cow)(void *ctx);
} cow_io_ops_t;

/* L2 Page Cache Way */
typedef struct {
    uint32_t l1_idx;             /* 0..active_l1_entries-1, or UINT32_MAX if invalid */
    uint64_t last_used_tick;     /* Monotonic tick for LRU eviction */
    uint32_t entries[COW_L2_ENTRIES]; /* 16 KB L2 page entries (slot index in pool, 0 = clean) */
} cow_l2_cache_way_t;

/* Complete cowMMC Engine State (Designed for static .bss allocation inside emummc.kip) */
typedef struct {
    bool               initialized;
    cow_io_ops_t       io;
    cow_header_t       hdr;
    uint32_t           active_sb_slot;        /* 0 or 1 */
    uint64_t           lru_tick;

    /* Resolved runtime geometry */
    uint64_t           physical_base_sectors; /* Underlying physical eMMC GPP sectors (matches gpp_total_sectors) */
    uint32_t           effective_total_slots; /* min(backing_slots, total_slots, max_overlay_slots) */
    uint32_t           active_l1_entries;     /* 1024..8192 */
    uint32_t           active_first_data_slot;/* l1_slot_b + ceil(l1_bytes/16K) */

    /* Full L1 Table in RAM (up to 32 KiB for 512 GiB virtual NAND) */
    uint32_t           l1_table[COW_L1_ENTRIES] __attribute__((aligned(8)));

    /* LRU Cache of L2 Pages in RAM (8 * 16 KiB = 128 KiB) */
    cow_l2_cache_way_t l2_cache[COW_L2_CACHE_WAYS] __attribute__((aligned(8)));

    /* 16 KiB Bounce Buffer for partial-block (< 32 sectors) copy-up writes */
    uint8_t            bounce_buf[COW_BLOCK_BYTES] __attribute__((aligned(16)));
} cow_engine_t;

/* Core Public API */
uint32_t cow_crc32(const void *data, size_t len);

/* Default-geometry formatter (formats a cowMMC v2 overlay matching physical base) */
bool cow_engine_format(
    const cow_io_ops_t *io,
    uint64_t gpp_total_sectors,
    uint32_t prefix_sectors,
    uint32_t total_slots,
    uint32_t flags
);

/* Full cowMMC v2 formatter with explicit physical base vs. virtual capacity and policy limits */
bool cow_engine_format_v2(
    const cow_io_ops_t *io,
    const cow_header_t *template_hdr
);

bool cow_engine_init(cow_engine_t *eng, const cow_io_ops_t *io);
bool cow_engine_init_bounded(cow_engine_t *eng, const cow_io_ops_t *io, uint32_t backing_slots, bool strict_base_check);
bool cow_engine_read_gpp(cow_engine_t *eng, uint32_t sector, uint32_t num_sectors, void *buf);
bool cow_engine_write_gpp(cow_engine_t *eng, uint32_t sector, uint32_t num_sectors, const void *buf);

#ifdef __cplusplus
}
#endif

#endif /* __COW_ENGINE_H__ */
