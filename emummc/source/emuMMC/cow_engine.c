/*
 * Copyright (c) 2026 HOS_COW Project
 *
 * GPLv2+ — Sparse Block-Level Copy-on-Write (COW) Engine for Atmosphère emummc.kip
 */

#include "cow_engine.h"
#include <string.h>

/* Incremental IEEE 802.3 CRC32 (Bitwise implementation: 0 bytes of lookup table in .rodata) */
static uint32_t _crc32_update(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return crc;
}

uint32_t cow_crc32(const void *data, size_t len)
{
    return ~_crc32_update(0xFFFFFFFFU, data, len);
}

static bool _validate_v2_slots(const cow_header_t *hdr)
{
    uint32_t l1_slots = COW_L1_SLOTS_FOR(hdr->l1_entries);
    if (l1_slots == 0U)
        return false;

    if (hdr->l1_slot_a < COW_SUPERBLOCK_SLOTS || hdr->l1_slot_b < COW_SUPERBLOCK_SLOTS)
        return false;

    if ((uint64_t)hdr->l1_slot_a + l1_slots > hdr->first_data_slot)
        return false;

    if ((uint64_t)hdr->l1_slot_b + l1_slots > hdr->first_data_slot)
        return false;

    bool non_overlapping =
        ((uint64_t)hdr->l1_slot_a + l1_slots <= hdr->l1_slot_b) ||
        ((uint64_t)hdr->l1_slot_b + l1_slots <= hdr->l1_slot_a);
    if (!non_overlapping)
        return false;

    if (hdr->total_slots <= hdr->first_data_slot)
        return false;

    if ((uint64_t)hdr->first_data_slot * COW_BLOCK_SECTORS > UINT32_MAX)
        return false;

    return true;
}

static bool _validate_header_integrity(
    const cow_header_t *hdr,
    uint64_t *out_phys_base,
    uint32_t *out_l1_entries,
    uint32_t *out_first_data_slot
)
{
    if (hdr->magic != COW_MAGIC)
        return false;

    uint32_t calc_hdr_crc = cow_crc32(hdr, offsetof(cow_header_t, header_crc32));
    if (calc_hdr_crc != hdr->header_crc32)
        return false;

    if (hdr->gpp_total_sectors == 0ULL)
        return false;

    if (hdr->version != COW_VERSION_V2)
        return false;

    if (hdr->header_ext_size != COW_SECTOR_SIZE ||
        hdr->block_shift != COW_BLOCK_SHIFT ||
        hdr->l2_shift != COW_L2_SHIFT)
    {
        return false;
    }

    if (hdr->l1_entries < COW_MIN_L1_ENTRIES || hdr->l1_entries > COW_L1_ENTRIES)
        return false;

    if (hdr->physical_base_sectors == 0ULL || hdr->gpp_total_sectors != hdr->physical_base_sectors)
        return false;

    if (!_validate_v2_slots(hdr))
        return false;

    uint64_t phys_base       = hdr->physical_base_sectors;
    uint32_t l1_entries      = hdr->l1_entries;
    uint32_t first_data_slot = hdr->first_data_slot;

    uint64_t max_l1_sectors = (uint64_t)l1_entries * COW_L2_ENTRIES * COW_BLOCK_SECTORS;
    if (max_l1_sectors < hdr->gpp_total_sectors)
        return false;

    if (hdr->next_free_slot < first_data_slot || hdr->next_free_slot > hdr->total_slots)
        return false;

    if ((uint64_t)hdr->next_free_slot * COW_BLOCK_SECTORS > UINT32_MAX)
        return false;

    *out_phys_base       = phys_base;
    *out_l1_entries      = l1_entries;
    *out_first_data_slot = first_data_slot;
    return true;
}

static uint32_t _resolve_effective_total_slots(
    const cow_header_t *hdr,
    uint32_t backing_slots
)
{
    uint32_t eff = hdr->total_slots;
    if (hdr->max_overlay_slots != 0U && hdr->max_overlay_slots < eff)
        eff = hdr->max_overlay_slots;
    if (backing_slots != 0U && backing_slots < eff)
        eff = backing_slots;
    return eff;
}

/* Load L1 table into eng->l1_table in <= 16 KiB chunks via eng->bounce_buf to keep stack tiny */
static bool _validate_and_load_l1(
    cow_engine_t *eng,
    const cow_header_t *hdr,
    uint32_t sb_slot
)
{
    uint32_t l1_entries   = hdr->l1_entries;
    uint32_t l1_bytes     = l1_entries * (uint32_t)sizeof(uint32_t);
    uint32_t l1_sectors   = (l1_bytes + COW_SECTOR_SIZE - 1U) / COW_SECTOR_SIZE;
    uint32_t start_sector = ((sb_slot == 0U) ? hdr->l1_slot_a : hdr->l1_slot_b) * COW_BLOCK_SECTORS;

    memset(eng->l1_table, 0, sizeof(eng->l1_table));

    uint32_t crc       = 0xFFFFFFFFU;
    uint32_t sect_done = 0U;
    uint32_t byte_done = 0U;
    uint8_t *dst       = (uint8_t *)eng->l1_table;

    while (sect_done < l1_sectors) {
        uint32_t chunk_sects = l1_sectors - sect_done;
        if (chunk_sects > COW_BLOCK_SECTORS)
            chunk_sects = COW_BLOCK_SECTORS;

        if (!eng->io.read_cow(eng->io.user_ctx, start_sector + sect_done, chunk_sects, eng->bounce_buf))
            return false;

        uint32_t chunk_bytes = chunk_sects * COW_SECTOR_SIZE;
        uint32_t rem_bytes   = l1_bytes - byte_done;
        if (chunk_bytes > rem_bytes)
            chunk_bytes = rem_bytes;

        memcpy(dst + byte_done, eng->bounce_buf, chunk_bytes);
        crc = _crc32_update(crc, eng->bounce_buf, chunk_bytes);

        sect_done += chunk_sects;
        byte_done += chunk_bytes;
    }

    return (~crc) == hdr->l1_crc32;
}

static bool _write_superblock_slot(
    const cow_io_ops_t *io,
    uint32_t sb_slot,
    cow_header_t *hdr,
    const uint32_t *l1_table,
    uint8_t *scratch_block_buf
)
{
    uint32_t l1_entries      = hdr->l1_entries;
    uint32_t l1_bytes        = l1_entries * (uint32_t)sizeof(uint32_t);
    uint32_t l1_sectors      = (l1_bytes + COW_SECTOR_SIZE - 1U) / COW_SECTOR_SIZE;
    uint32_t sb_sector       = sb_slot * COW_BLOCK_SECTORS;
    uint32_t l1_start_sector = ((sb_slot == 0U) ? hdr->l1_slot_a : hdr->l1_slot_b) * COW_BLOCK_SECTORS;

    if (l1_table != NULL && scratch_block_buf != NULL) {
        hdr->l1_crc32     = cow_crc32(l1_table, l1_bytes);
        hdr->header_crc32 = cow_crc32(hdr, offsetof(cow_header_t, header_crc32));

        /* Step 3a: Write the L1 table in <= 16 KiB DMA-safe chunks first */
        const uint8_t *src = (const uint8_t *)l1_table;
        uint32_t sect_done = 0U;
        uint32_t byte_done = 0U;

        while (sect_done < l1_sectors) {
            uint32_t chunk_sects = l1_sectors - sect_done;
            if (chunk_sects > COW_BLOCK_SECTORS)
                chunk_sects = COW_BLOCK_SECTORS;

            uint32_t chunk_bytes = chunk_sects * COW_SECTOR_SIZE;
            uint32_t rem_bytes   = l1_bytes - byte_done;
            uint32_t copy_bytes  = (chunk_bytes < rem_bytes) ? chunk_bytes : rem_bytes;

            if (copy_bytes < chunk_bytes)
                memset(scratch_block_buf, 0, chunk_bytes);
            memcpy(scratch_block_buf, src + byte_done, copy_bytes);

            if (!io->write_cow(io->user_ctx, l1_start_sector + sect_done, chunk_sects, scratch_block_buf))
                return false;

            sect_done += chunk_sects;
            byte_done += copy_bytes;
        }
    } else {
        /* Format path (all-zero L1): keep stack footprint minimal (512B sector buffer) */
        uint8_t zero_sec[COW_SECTOR_SIZE];
        memset(zero_sec, 0, sizeof(zero_sec));

        uint32_t crc = 0xFFFFFFFFU;
        for (uint32_t rem = l1_bytes; rem > 0U;) {
            uint32_t cur = (rem > COW_SECTOR_SIZE) ? COW_SECTOR_SIZE : rem;
            crc = _crc32_update(crc, zero_sec, cur);
            rem -= cur;
        }
        hdr->l1_crc32     = ~crc;
        hdr->header_crc32 = cow_crc32(hdr, offsetof(cow_header_t, header_crc32));

        for (uint32_t s = 0U; s < l1_sectors; s++) {
            if (!io->write_cow(io->user_ctx, l1_start_sector + s, 1U, zero_sec))
                return false;
        }
    }

    /* Step 3b: Write the 512B header (sector 0 of the superblock slot) last to commit */
    if (!io->write_cow(io->user_ctx, sb_sector, 1U, hdr))
        return false;

    if (io->sync_cow && !io->sync_cow(io->user_ctx))
        return false;

    return true;
}

static void _reset_l2_cache(cow_engine_t *eng)
{
    eng->lru_tick = 0ULL;
    for (uint32_t i = 0U; i < COW_L2_CACHE_WAYS; i++) {
        eng->l2_cache[i].l1_idx = UINT32_MAX;
        eng->l2_cache[i].last_used_tick = 0ULL;
    }
}

bool cow_engine_format(
    const cow_io_ops_t *io,
    uint64_t gpp_total_sectors,
    uint32_t prefix_sectors,
    uint32_t total_slots,
    uint32_t flags
)
{
    cow_header_t tmpl;
    memset(&tmpl, 0, sizeof(tmpl));

    tmpl.gpp_total_sectors     = gpp_total_sectors;
    tmpl.physical_base_sectors = gpp_total_sectors;
    tmpl.prefix_sectors        = prefix_sectors;
    tmpl.total_slots           = total_slots;
    tmpl.flags                 = flags;
    tmpl.alloc_policy          = (flags & COW_FLAG_THIN_PROVISION) ? COW_ALLOC_THIN : COW_ALLOC_THICK;

    return cow_engine_format_v2(io, &tmpl);
}

bool cow_engine_format_v2(
    const cow_io_ops_t *io,
    const cow_header_t *template_hdr
)
{
    if (!io || !io->write_cow || !template_hdr)
        return false;

    cow_header_t hdr = *template_hdr;

    if (hdr.header_ext_size != 0U && hdr.header_ext_size != COW_SECTOR_SIZE)
        return false;
    hdr.header_ext_size = COW_SECTOR_SIZE;

    if (hdr.block_shift != 0U && hdr.block_shift != COW_BLOCK_SHIFT)
        return false;
    hdr.block_shift = COW_BLOCK_SHIFT;

    if (hdr.l2_shift != 0U && hdr.l2_shift != COW_L2_SHIFT)
        return false;
    hdr.l2_shift = COW_L2_SHIFT;

    if (hdr.gpp_total_sectors == 0ULL)
        return false;

    if (hdr.physical_base_sectors == 0ULL)
        hdr.physical_base_sectors = hdr.gpp_total_sectors;

    if (hdr.gpp_total_sectors != hdr.physical_base_sectors)
        return false;

    if (hdr.l1_entries == 0U) {
        uint64_t sectors_per_l1 = (uint64_t)COW_L2_ENTRIES * COW_BLOCK_SECTORS;
        uint64_t needed_l1 = (hdr.gpp_total_sectors + sectors_per_l1 - 1ULL) / sectors_per_l1;
        if (needed_l1 < COW_MIN_L1_ENTRIES)
            needed_l1 = COW_MIN_L1_ENTRIES;
        needed_l1 = (needed_l1 + (COW_L2_ENTRIES_PER_SECT - 1U)) & ~((uint64_t)COW_L2_ENTRIES_PER_SECT - 1ULL);
        if (needed_l1 > COW_L1_ENTRIES)
            return false;
        hdr.l1_entries = (uint32_t)needed_l1;
    }

    if (hdr.l1_entries < COW_MIN_L1_ENTRIES || hdr.l1_entries > COW_L1_ENTRIES)
        return false;

    if (((uint64_t)hdr.l1_entries * COW_L2_ENTRIES * COW_BLOCK_SECTORS) < hdr.gpp_total_sectors)
        return false;

    uint32_t l1_slots = COW_L1_SLOTS_FOR(hdr.l1_entries);
    if (hdr.l1_slot_a == 0U && hdr.l1_slot_b == 0U && hdr.first_data_slot == 0U) {
        hdr.l1_slot_a       = COW_SUPERBLOCK_SLOTS;
        hdr.l1_slot_b       = hdr.l1_slot_a + l1_slots;
        hdr.first_data_slot = hdr.l1_slot_b + l1_slots;
    }

    if (!_validate_v2_slots(&hdr))
        return false;

    if (hdr.max_overlay_slots != 0U && hdr.total_slots > hdr.max_overlay_slots)
        return false;

    hdr.magic             = COW_MAGIC;
    hdr.version           = COW_VERSION_V2;
    hdr.seq_num           = 1ULL;
    hdr.next_free_slot    = hdr.first_data_slot;
    hdr.dirty_data_blocks = 0U;
    hdr.dirty_l2_pages    = 0U;

    /* Write SB0 (seq 1, L1 in l1_slot_a) and SB1 (seq 2, L1 in l1_slot_b) */
    if (!_write_superblock_slot(io, 0U, &hdr, NULL, NULL))
        return false;

    hdr.seq_num = 2ULL;
    if (!_write_superblock_slot(io, 1U, &hdr, NULL, NULL))
        return false;

    return true;
}

/* Sanitize committed L2 pages on mount to clear any uncommitted entries >= next_free_slot
 * left behind by a power loss between Step 2 (L2 sector write) and Step 3 (Superblock commit).
 */
static bool _sanitize_committed_l2_pages(cow_engine_t *eng)
{
    const uint32_t first_data = eng->active_first_data_slot;
    const uint32_t limit_slot = eng->hdr.next_free_slot;
    uint32_t *page_buf = eng->l2_cache[0].entries;

    for (uint32_t l1_idx = 0U; l1_idx < eng->active_l1_entries; l1_idx++) {
        uint32_t l2_slot = eng->l1_table[l1_idx];
        if (l2_slot == 0U)
            continue;

        if (l2_slot < first_data || l2_slot >= limit_slot) {
            /* Invalid L1 entry (should never happen with valid L1 CRC) */
            return false;
        }

        uint32_t l2_sector = l2_slot * COW_BLOCK_SECTORS;
        if (!eng->io.read_cow(eng->io.user_ctx, l2_sector, COW_BLOCK_SECTORS, page_buf))
            return false;

        bool modified = false;
        for (uint32_t i = 0U; i < COW_L2_ENTRIES; i++) {
            uint32_t data_slot = page_buf[i];
            if (data_slot == 0U)
                continue;
            if (data_slot < first_data)
                return false;
            if (data_slot >= limit_slot) {
                page_buf[i] = 0U;
                modified = true;
            }
        }

        if (modified) {
            if (!eng->io.write_cow(eng->io.user_ctx, l2_sector, COW_BLOCK_SECTORS, page_buf))
                return false;
        }
    }

    if (eng->io.sync_cow && !eng->io.sync_cow(eng->io.user_ctx))
        return false;

    return true;
}

bool cow_engine_init_bounded(cow_engine_t *eng, const cow_io_ops_t *io, uint32_t backing_slots, bool strict_base_check)
{
    if (!eng || !io || !io->read_base_gpp || !io->read_cow || !io->write_cow)
        return false;

    memset(eng, 0, sizeof(*eng));
    eng->io = *io;
    _reset_l2_cache(eng);

    cow_header_t hdrs[COW_SUPERBLOCK_SLOTS];
    uint64_t phys_base[COW_SUPERBLOCK_SLOTS] = {0ULL, 0ULL};
    uint32_t l1_entries[COW_SUPERBLOCK_SLOTS] = {0U, 0U};
    uint32_t first_data[COW_SUPERBLOCK_SLOTS] = {0U, 0U};
    bool hdr_ok[COW_SUPERBLOCK_SLOTS] = {false, false};

    for (uint32_t s = 0U; s < COW_SUPERBLOCK_SLOTS; s++) {
        if (io->read_cow(io->user_ctx, s * COW_BLOCK_SECTORS, 1U, &hdrs[s])) {
            hdr_ok[s] = _validate_header_integrity(&hdrs[s], &phys_base[s], &l1_entries[s], &first_data[s]);
        }
    }

    if (!hdr_ok[0] && !hdr_ok[1])
        return false;

    /* Try the highest-sequence valid header first; fall back to the other slot only if L1 fails CRC */
    uint32_t order[COW_SUPERBLOCK_SLOTS];
    if (hdr_ok[0] && (!hdr_ok[1] || hdrs[0].seq_num >= hdrs[1].seq_num)) {
        order[0] = 0U;
        order[1] = 1U;
    } else {
        order[0] = 1U;
        order[1] = 0U;
    }

    bool mounted = false;
    for (uint32_t i = 0U; i < COW_SUPERBLOCK_SLOTS; i++) {
        uint32_t s = order[i];
        if (!hdr_ok[s])
            continue;

        if (_validate_and_load_l1(eng, &hdrs[s], s)) {
            eng->hdr                    = hdrs[s];
            eng->active_sb_slot         = s;
            eng->physical_base_sectors  = phys_base[s];
            eng->active_l1_entries      = l1_entries[s];
            eng->active_first_data_slot = first_data[s];
            mounted = true;
            break;
        }
    }

    if (!mounted) {
        memset(eng, 0, sizeof(*eng));
        return false;
    }

    eng->effective_total_slots = _resolve_effective_total_slots(&eng->hdr, backing_slots);
    if (eng->effective_total_slots <= eng->active_first_data_slot ||
        eng->hdr.next_free_slot > eng->effective_total_slots)
    {
        memset(eng, 0, sizeof(*eng));
        return false;
    }

    if (strict_base_check && (eng->hdr.flags & (COW_FLAG_FW_MISMATCH | COW_FLAG_CONFLICT_LOCKED))) {
        memset(eng, 0, sizeof(*eng));
        return false;
    }

    _reset_l2_cache(eng);

    /* If Ephemeral Mode is enabled, wipe all overlay mappings on boot while preserving v2 geometry & metadata */
    if (eng->hdr.flags & COW_FLAG_EPHEMERAL) {
        memset(eng->l1_table, 0, sizeof(eng->l1_table));
        eng->hdr.next_free_slot    = eng->active_first_data_slot;
        eng->hdr.dirty_data_blocks = 0U;
        eng->hdr.dirty_l2_pages    = 0U;
        eng->hdr.seq_num++;
        uint32_t next_sb_slot = eng->active_sb_slot ^ 1U;

        if (!_write_superblock_slot(&eng->io, next_sb_slot, &eng->hdr, eng->l1_table, eng->bounce_buf)) {
            memset(eng, 0, sizeof(*eng));
            return false;
        }
        eng->active_sb_slot = next_sb_slot;
    } else {
        /* Persistent mode: scrub any uncommitted L2 entries from prior power loss */
        if (!_sanitize_committed_l2_pages(eng)) {
            memset(eng, 0, sizeof(*eng));
            return false;
        }
        _reset_l2_cache(eng);
    }

    eng->initialized = true;
    return true;
}

bool cow_engine_init(cow_engine_t *eng, const cow_io_ops_t *io)
{
    return cow_engine_init_bounded(eng, io, 0U, false);
}

/* Fetch an L2 page into the in-RAM LRU cache (only called when l1_table[l1_idx] != 0) */
static cow_l2_cache_way_t *_get_l2_cache_way(cow_engine_t *eng, uint32_t l1_idx)
{
    /* 1. Check for cache hit */
    for (uint32_t i = 0U; i < COW_L2_CACHE_WAYS; i++) {
        if (eng->l2_cache[i].l1_idx == l1_idx) {
            eng->l2_cache[i].last_used_tick = ++eng->lru_tick;
            return &eng->l2_cache[i];
        }
    }

    /* 2. Cache miss: pick invalid way or oldest LRU way */
    uint32_t victim = 0U;
    for (uint32_t i = 0U; i < COW_L2_CACHE_WAYS; i++) {
        if (eng->l2_cache[i].l1_idx == UINT32_MAX) {
            victim = i;
            break;
        }
        if (eng->l2_cache[i].last_used_tick < eng->l2_cache[victim].last_used_tick) {
            victim = i;
        }
    }

    uint32_t l2_slot = eng->l1_table[l1_idx];
    uint32_t l2_sector = l2_slot * COW_BLOCK_SECTORS;

    if (!eng->io.read_cow(eng->io.user_ctx, l2_sector, COW_BLOCK_SECTORS, eng->l2_cache[victim].entries))
        return NULL;

    eng->l2_cache[victim].l1_idx = l1_idx;
    eng->l2_cache[victim].last_used_tick = ++eng->lru_tick;
    return &eng->l2_cache[victim];
}

/* Look up the COW0 slot for a 16 KB GPP block (returns 0 if clean, UINT32_MAX on I/O error) */
static uint32_t _lookup_block_slot(cow_engine_t *eng, uint32_t block_idx)
{
    uint32_t l1_idx = block_idx >> COW_L2_SHIFT;
    uint32_t l2_idx = block_idx & COW_L2_MASK;

    if (l1_idx >= eng->active_l1_entries)
        return UINT32_MAX;

    if (eng->l1_table[l1_idx] == 0U)
        return 0U; /* Fast path: entire 64 MB region is untouched */

    cow_l2_cache_way_t *way = _get_l2_cache_way(eng, l1_idx);
    if (!way)
        return UINT32_MAX;

    return way->entries[l2_idx];
}

/* Read clean (unmodified) sectors across 2 physical spans:
 * 1) [sector, min(end, prefix_sectors))               -> io.read_prefix
 * 2) [max(sector, prefix_sectors), min(end, base))    -> io.read_base_gpp
 */
static bool _read_clean_gpp_span(cow_engine_t *eng, uint32_t sector, uint32_t num_sectors, void *buf)
{
    uint8_t *out = (uint8_t *)buf;
    uint64_t end = (uint64_t)sector + num_sectors;

    if (eng->io.read_prefix != NULL && (uint64_t)sector < eng->hdr.prefix_sectors) {
        uint64_t prefix_end = eng->hdr.prefix_sectors;
        if (prefix_end > end)
            prefix_end = end;

        uint32_t prefix_cnt = (uint32_t)(prefix_end - sector);
        if (!eng->io.read_prefix(eng->io.user_ctx, sector, prefix_cnt, out))
            return false;

        out    += (size_t)prefix_cnt * COW_SECTOR_SIZE;
        sector += prefix_cnt;
    }

    if ((uint64_t)sector < eng->physical_base_sectors && (uint64_t)sector < end) {
        uint64_t base_end = eng->physical_base_sectors;
        if (base_end > end)
            base_end = end;

        uint32_t base_cnt = (uint32_t)(base_end - sector);
        if (!eng->io.read_base_gpp(eng->io.user_ctx, sector, base_cnt, out))
            return false;

        out    += (size_t)base_cnt * COW_SECTOR_SIZE;
        sector += base_cnt;
    }

    if ((uint64_t)sector < end)
        return false;

    return true;
}

bool cow_engine_read_gpp(cow_engine_t *eng, uint32_t sector, uint32_t num_sectors, void *buf)
{
    if (!eng || !eng->initialized || !buf)
        return false;

    if (num_sectors == 0U)
        return true;

    if ((uint64_t)sector + num_sectors > eng->hdr.gpp_total_sectors)
        return false;

    uint8_t *out = (uint8_t *)buf;

    /* Process all sectors (including prefix_sectors) across 16 KB (32-sector) COW blocks,
     * coalescing contiguous clean runs for maximum DMA throughput.
     */
    uint32_t clean_run_start = 0U;
    uint32_t clean_run_count = 0U;
    uint8_t *clean_run_buf   = NULL;

    while (num_sectors > 0U) {
        uint32_t block_idx = sector >> COW_BLOCK_SHIFT;
        uint32_t sect_in_block = sector & COW_BLOCK_MASK;
        uint32_t chunk_sectors = COW_BLOCK_SECTORS - sect_in_block;
        if (chunk_sectors > num_sectors)
            chunk_sectors = num_sectors;

        uint32_t data_slot = _lookup_block_slot(eng, block_idx);
        if (data_slot == UINT32_MAX)
            return false;

        if (data_slot == 0U) {
            /* Clean block: coalesce contiguous clean reads */
            if (clean_run_count == 0U) {
                clean_run_start = sector;
                clean_run_buf   = out;
            }
            clean_run_count += chunk_sectors;
        } else {
            /* Flush any pending contiguous clean run first */
            if (clean_run_count > 0U) {
                if (!_read_clean_gpp_span(eng, clean_run_start, clean_run_count, clean_run_buf))
                    return false;
                clean_run_count = 0U;
            }

            /* Read dirty chunk from COW0 slot */
            uint32_t cow_sector = (data_slot * COW_BLOCK_SECTORS) + sect_in_block;
            if (!eng->io.read_cow(eng->io.user_ctx, cow_sector, chunk_sectors, out))
                return false;
        }

        out         += (size_t)chunk_sectors * COW_SECTOR_SIZE;
        sector      += chunk_sectors;
        num_sectors -= chunk_sectors;
    }

    if (clean_run_count > 0U) {
        if (!_read_clean_gpp_span(eng, clean_run_start, clean_run_count, clean_run_buf))
            return false;
    }

    return true;
}

bool cow_engine_write_gpp(cow_engine_t *eng, uint32_t sector, uint32_t num_sectors, const void *buf)
{
    if (!eng || !eng->initialized || !buf)
        return false;

    if (num_sectors == 0U)
        return true;

    if ((uint64_t)sector + num_sectors > eng->hdr.gpp_total_sectors)
        return false;

    /* Pre-flight atomic capacity check across all blocks in the span before mutating
     * any COW0 slots, RAM L1 entries, or superblock counters.
     */
    uint32_t start_block = sector >> COW_BLOCK_SHIFT;
    uint32_t end_block   = (sector + num_sectors - 1U) >> COW_BLOCK_SHIFT;
    if ((end_block >> COW_L2_SHIFT) >= eng->active_l1_entries)
        return false;

    uint32_t avail_slots = (eng->hdr.next_free_slot < eng->effective_total_slots)
                           ? (eng->effective_total_slots - eng->hdr.next_free_slot)
                           : 0U;
    uint64_t total_slots_needed = 0ULL;
    uint32_t blk = start_block;
    while (blk <= end_block) {
        uint32_t l1_idx = blk >> COW_L2_SHIFT;
        if (eng->l1_table[l1_idx] == 0U) {
            uint32_t region_end = ((l1_idx + 1U) << COW_L2_SHIFT) - 1U;
            if (region_end > end_block)
                region_end = end_block;

            total_slots_needed += 1ULL + (uint64_t)(region_end - blk + 1U);
            if (total_slots_needed > avail_slots)
                return false;

            if (region_end == end_block)
                break;
            blk = region_end + 1U;
        } else {
            uint32_t data_slot = _lookup_block_slot(eng, blk);
            if (data_slot == UINT32_MAX)
                return false;

            if (data_slot == 0U) {
                total_slots_needed += 1ULL;
                if (total_slots_needed > avail_slots)
                    return false;
            }

            if (blk == end_block)
                break;
            blk++;
        }
    }

    const uint8_t *in = (const uint8_t *)buf;
    bool allocated_any = false;

    /* Process all sectors (including prefix_sectors < 32 MB) via the 16 KB COW pool so that
     * the 32 MB bootloader prefix in /eMMC/00 remains 100% immutable and revertible.
     */
    while (num_sectors > 0U) {
        uint32_t block_idx     = sector >> COW_BLOCK_SHIFT;
        uint32_t sect_in_block = sector & COW_BLOCK_MASK;
        uint32_t chunk_sectors = COW_BLOCK_SECTORS - sect_in_block;
        if (chunk_sectors > num_sectors)
            chunk_sectors = num_sectors;

        uint32_t l1_idx = block_idx >> COW_L2_SHIFT;
        uint32_t l2_idx = block_idx & COW_L2_MASK;
        if (l1_idx >= eng->active_l1_entries)
            return false;

        uint32_t data_slot = _lookup_block_slot(eng, block_idx);
        if (data_slot == UINT32_MAX)
            return false;

        if (data_slot != 0U) {
            /* Fast Path: 16 KB block is already allocated in COW0! Write modified sectors in-place. */
            uint32_t cow_sector = (data_slot * COW_BLOCK_SECTORS) + sect_in_block;
            if (!eng->io.write_cow(eng->io.user_ctx, cow_sector, chunk_sectors, in))
                return false;
        } else {
            /* First write to this 16 KB block: verify capacity BEFORE allocating (strictly fail-closed) */
            bool need_new_l2 = (eng->l1_table[l1_idx] == 0U);
            uint32_t slots_needed = need_new_l2 ? 2U : 1U;
            if ((uint64_t)eng->hdr.next_free_slot + slots_needed > eng->effective_total_slots)
                return false; /* Overlay full — fail closed, never touch physical sysMMC! */

            uint32_t new_l2_slot   = need_new_l2 ? eng->hdr.next_free_slot : eng->l1_table[l1_idx];
            uint32_t new_data_slot = need_new_l2 ? (eng->hdr.next_free_slot + 1U) : eng->hdr.next_free_slot;

            /* Step 1 (Data First): Prepare and write the full 16 KB data block to new_data_slot */
            uint32_t data_cow_sector = new_data_slot * COW_BLOCK_SECTORS;
            if (chunk_sectors == COW_BLOCK_SECTORS) {
                /* Full 16 KB aligned write — no copy-up read from base/prefix needed! */
                if (!eng->io.write_cow(eng->io.user_ctx, data_cow_sector, COW_BLOCK_SECTORS, in))
                    return false;
            } else {
                /* Partial 16 KB write — copy-up full 16 KB block from clean base/prefix/zero-fill first */
                uint32_t base_block_sector = block_idx * COW_BLOCK_SECTORS;
                if (!_read_clean_gpp_span(eng, base_block_sector, COW_BLOCK_SECTORS, eng->bounce_buf))
                    return false;

                memcpy(
                    eng->bounce_buf + ((size_t)sect_in_block * COW_SECTOR_SIZE),
                    in,
                    (size_t)chunk_sectors * COW_SECTOR_SIZE
                );

                if (!eng->io.write_cow(eng->io.user_ctx, data_cow_sector, COW_BLOCK_SECTORS, eng->bounce_buf))
                    return false;
            }

            /* Step 2 (L2 Table Page Second): Update L2 page in cache and flush to COW0 */
            if (need_new_l2) {
                /* Claim a cache way for the newly allocated L2 page */
                uint32_t victim = 0U;
                for (uint32_t i = 0U; i < COW_L2_CACHE_WAYS; i++) {
                    if (eng->l2_cache[i].l1_idx == UINT32_MAX) {
                        victim = i;
                        break;
                    }
                    if (eng->l2_cache[i].last_used_tick < eng->l2_cache[victim].last_used_tick) {
                        victim = i;
                    }
                }

                cow_l2_cache_way_t *way = &eng->l2_cache[victim];
                memset(way->entries, 0, sizeof(way->entries));
                way->entries[l2_idx] = new_data_slot;
                way->l1_idx          = l1_idx;
                way->last_used_tick  = ++eng->lru_tick;

                /* Write the full 16 KB newly allocated L2 page to new_l2_slot */
                if (!eng->io.write_cow(eng->io.user_ctx, new_l2_slot * COW_BLOCK_SECTORS, COW_BLOCK_SECTORS, way->entries))
                    return false;

                eng->l1_table[l1_idx]    = new_l2_slot;
                eng->hdr.next_free_slot += 2U;
                eng->hdr.dirty_l2_pages += 1U;
            } else {
                cow_l2_cache_way_t *way = _get_l2_cache_way(eng, l1_idx);
                if (!way)
                    return false;

                way->entries[l2_idx] = new_data_slot;

                /* Write only the single dirty 512B sector of the existing L2 page */
                uint32_t l2_sect_off = l2_idx / COW_L2_ENTRIES_PER_SECT;
                uint32_t l2_cow_sect = (new_l2_slot * COW_BLOCK_SECTORS) + l2_sect_off;
                const void *l2_sect_ptr = &way->entries[l2_sect_off * COW_L2_ENTRIES_PER_SECT];

                if (!eng->io.write_cow(eng->io.user_ctx, l2_cow_sect, 1U, l2_sect_ptr))
                    return false;

                eng->hdr.next_free_slot += 1U;
            }

            eng->hdr.dirty_data_blocks += 1U;
            allocated_any = true;
        }

        in          += (size_t)chunk_sectors * COW_SECTOR_SIZE;
        sector      += chunk_sectors;
        num_sectors -= chunk_sectors;
    }

    /* Step 3 (Ping-Pong Superblock Last): Commit only if new slots were allocated */
    if (allocated_any) {
        eng->hdr.seq_num++;
        uint32_t next_sb_slot = eng->active_sb_slot ^ 1U;
        if (!_write_superblock_slot(&eng->io, next_sb_slot, &eng->hdr, eng->l1_table, eng->bounce_buf))
            return false;
        eng->active_sb_slot = next_sb_slot;
    }

    return true;
}
