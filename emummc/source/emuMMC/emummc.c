/*
 * Copyright (c) 2019 m4xw <m4x@m4xw.net>
 * Copyright (c) 2019 Atmosphere-NX
 * Copyright (c) 2019 CTCaer
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>

#include "emummc.h"
#include "emummc_ctx.h"
#include "cow_engine.h"
#include "../nx/svc.h"
#include "../utils/fatal.h"
#include "../libs/fatfs/diskio.h"

static bool sdmmc_first_init = false;
static bool storageSDinitialized = false;

// hekate sdmmmc vars
sdmmc_t sdmmc;
sdmmc_storage_t storage;
sdmmc_t sd_sdmmc;
sdmmc_storage_t sd_storage;

// init vars
bool init_done = false;
bool custom_driver = true;

// FS funcs
_sdmmc_accessor_gc sdmmc_accessor_gc;
_sdmmc_accessor_sd sdmmc_accessor_sd;
_sdmmc_accessor_nand sdmmc_accessor_nand;
_lock_mutex lock_mutex;
_unlock_mutex unlock_mutex;

// FS misc
void *sd_mutex;
void *nand_mutex;
volatile int *active_partition;
volatile Handle *sdmmc_das_handle;

// FatFS
file_based_ctxt f_emu;
static bool fat_mounted = false;

// cowMMC state
static cow_engine_t g_cow_engine;
static uint32_t g_cow_base_sector = COW_DEFAULT_PREFIX_SECTS;
static uint64_t g_raw_file_total_sect = 0;
static uint8_t g_dma_save_buf[COW_BLOCK_BYTES] __attribute__((aligned(16)));

typedef struct {
    sdmmc_accessor_vt_t vt;
    void *ext[8]; /* Preserve GetAndClearErrorInfo + BaseDeviceAccessor virtuals (OnActivate/OnReadWrite/ReStartup) */
} cow_nand_vtab_shadow_t;

_Static_assert(offsetof(cow_nand_vtab_shadow_t, vt) == 0, "cow_nand_vtab_shadow_t layout");

static cow_nand_vtab_shadow_t g_cow_nand_vtab;
static bool g_cow_nand_vtab_installed = false;

static uint64_t _cow_nand_get_memory_capacity(void *_this, uint32_t *out_sectors)
{
    (void)_this;
    if (out_sectors != NULL)
        *out_sectors = (uint32_t)g_cow_engine.hdr.gpp_total_sectors;
    return 0;
}

static bool _cow_ptr_has_perm(uintptr_t ptr, size_t len, u32 required_perm)
{
    if (ptr < 0x10000ULL)
        return false;
    MemoryInfo mi;
    u32 pi = 0;
    if (svcQueryMemory(&mi, &pi, (u64)ptr) != 0)
        return false;
    if ((mi.perm & required_perm) != required_perm)
        return false;
    if ((u64)ptr < mi.addr || ((u64)ptr - mi.addr) + (u64)len > mi.size)
        return false;
    return true;
}

static void _cow_apply_capacity_override(sdmmc_accessor_t *_this)
{
    if (_this != NULL &&
        g_cow_engine.initialized &&
        g_cow_engine.hdr.gpp_total_sectors > g_cow_engine.physical_base_sectors)
    {
        if (_cow_ptr_has_perm((uintptr_t)_this->vtab, sizeof(g_cow_nand_vtab), Perm_R))
        {
            if (!g_cow_nand_vtab_installed)
            {
                memcpy(&g_cow_nand_vtab, _this->vtab, sizeof(g_cow_nand_vtab));
                g_cow_nand_vtab.vt.get_memory_capacity = _cow_nand_get_memory_capacity;
                g_cow_nand_vtab_installed = true;
            }
            _this->vtab = &g_cow_nand_vtab.vt;
        }

        uintptr_t base_dev_ptr = 0;
        if (_cow_ptr_has_perm((uintptr_t)_this + 0x10, sizeof(uintptr_t), Perm_R))
            base_dev_ptr = *(const uintptr_t *)((uintptr_t)_this + 0x10);
        if (_cow_ptr_has_perm(base_dev_ptr, 0x30, Perm_Rw))
        {
            uint32_t *base_dev = (uint32_t *)base_dev_ptr;
            if (base_dev[0x24 / 4] == (uint32_t)g_cow_engine.physical_base_sectors)
                base_dev[0x24 / 4] = (uint32_t)g_cow_engine.hdr.gpp_total_sectors;
        }
    }
}

static bool _file_based_gpp_raw_rw_inner(void *buf, unsigned int sector, unsigned int num_sectors, bool is_write)
{
    FIL *fp = NULL;

    if (__builtin_expect((uint64_t)sector + num_sectors > g_raw_file_total_sect, 0))
        return false;

    if (f_emu.parts)
    {
        fp = &f_emu.fp_gpp[sector / f_emu.part_size];
        sector = sector % f_emu.part_size;

        // Special handling for reads/writes which cross split file boundaries.
        if (__builtin_expect(sector + num_sectors > f_emu.part_size, 0))
        {
            unsigned int remaining = num_sectors;
            while (remaining > 0) {
                const unsigned int cur_sectors = MIN(remaining, f_emu.part_size - sector);

                if (f_lseek(fp, (uint64_t)sector << 9) != FR_OK)
                    return false;

                if (!is_write)
                {
                    if (f_read_fast(fp, buf, (uint64_t)cur_sectors << 9) != FR_OK)
                        return false;
                }
                else
                {
                    if (f_write_fast(fp, buf, (uint64_t)cur_sectors << 9) != FR_OK)
                        return false;
                }

                buf = (char *)buf + ((uint64_t)cur_sectors << 9);
                remaining -= cur_sectors;
                sector = 0;
                ++fp;
            }

            return true;
        }
    }
    else
    {
        fp = &f_emu.fp_gpp[0];
    }

    if (f_lseek(fp, (uint64_t)sector << 9) != FR_OK)
        return false;

    if (!is_write)
        return (f_read_fast(fp, buf, (uint64_t)num_sectors << 9) == FR_OK);
    else
        return (f_write_fast(fp, buf, (uint64_t)num_sectors << 9) == FR_OK);
}

static unsigned int _cow_dma_chunk_sectors(sdmmc_accessor_t *acc, int dma_idx, unsigned int rem)
{
    uint64_t dma_cap = acc->parent->dmaBuffers[dma_idx].device_addr_buffer_size;
    unsigned int cap_sects = (unsigned int)(dma_cap >> 9);
    if (cap_sects == 0)
        cap_sects = 1;
    if (cap_sects > COW_BLOCK_SECTORS)
        cap_sects = COW_BLOCK_SECTORS;
    return (rem < cap_sects) ? rem : cap_sects;
}

static bool _file_based_gpp_raw_rw(void *buf, unsigned int sector, unsigned int num_sectors, bool is_write)
{
    /* When cow_engine reads/writes internal .bss buffers (superblock, L1, L2 cache way,
     * or bounce_buf), stage through FS.kip's DMA buffer while saving and restoring its
     * contents so an in-flight multi-sector FS.kip DMA buffer is never clobbered.
     */
    if (_current_accessor != NULL &&
        num_sectors <= COW_BLOCK_SECTORS &&
        !sdmmc_calculate_dma_addr(_current_accessor, buf, num_sectors))
    {
        int dma_idx = sdmmc_calculate_fitting_dma_index(_current_accessor, num_sectors);
        void *dma_buf = &_current_accessor->parent->dmaBuffers[dma_idx].device_addr_buffer[0];
        if (dma_buf == NULL)
            return false;

        uint8_t *p = (uint8_t *)buf;
        unsigned int rem = num_sectors;
        while (rem > 0)
        {
            unsigned int cur_sects = _cow_dma_chunk_sectors(_current_accessor, dma_idx, rem);
            uint64_t bytes = (uint64_t)cur_sects << 9;

            memcpy(g_dma_save_buf, dma_buf, bytes);
            if (is_write)
                memcpy(dma_buf, p, bytes);

            bool ok = _file_based_gpp_raw_rw_inner(dma_buf, sector, cur_sects, is_write);

            if (!is_write && ok)
                memcpy(p, dma_buf, bytes);
            memcpy(dma_buf, g_dma_save_buf, bytes);

            if (!ok)
                return false;

            p      += bytes;
            sector += cur_sects;
            rem    -= cur_sects;
        }
        return true;
    }

    return _file_based_gpp_raw_rw_inner(buf, sector, num_sectors, is_write);
}

static bool _cow_read_base_gpp(void *ctx, uint32_t sector, uint32_t num_sectors, void *buf)
{
    (void)ctx;
    sdmmc_accessor_t *accessor_nand = sdmmc_accessor_nand();
    uint64_t bytes = (uint64_t)num_sectors << 9;

    if (sdmmc_calculate_dma_addr(accessor_nand, buf, num_sectors))
    {
        return (accessor_nand->vtab->read_write(accessor_nand, sector, num_sectors, buf, bytes, 1) == 0);
    }
    else
    {
        /* Copy-up bounce buffer lies in .bss; stage through FS.kip's mapped NAND DMA buffer */
        int dma_idx = sdmmc_calculate_fitting_dma_index(accessor_nand, num_sectors);
        void *dma_buf = &accessor_nand->parent->dmaBuffers[dma_idx].device_addr_buffer[0];
        if (dma_buf == NULL)
            return false;

        uint8_t *p = (uint8_t *)buf;
        unsigned int rem = num_sectors;
        while (rem > 0)
        {
            unsigned int cur_sects = _cow_dma_chunk_sectors(accessor_nand, dma_idx, rem);
            uint64_t cur_bytes = (uint64_t)cur_sects << 9;

            memcpy(g_dma_save_buf, dma_buf, cur_bytes);
            uint64_t rc = accessor_nand->vtab->read_write(accessor_nand, sector, cur_sects, dma_buf, cur_bytes, 1);
            if (rc == 0)
                memcpy(p, dma_buf, cur_bytes);
            memcpy(dma_buf, g_dma_save_buf, cur_bytes);

            if (rc != 0)
                return false;

            p      += cur_bytes;
            sector += cur_sects;
            rem    -= cur_sects;
        }
        return true;
    }
}

static bool _cow_read_prefix(void *ctx, uint32_t sector, uint32_t num_sectors, void *buf)
{
    (void)ctx;
    return _file_based_gpp_raw_rw(buf, sector, num_sectors, false);
}

static bool _cow_write_prefix(void *ctx, uint32_t sector, uint32_t num_sectors, const void *buf)
{
    (void)ctx;
    return _file_based_gpp_raw_rw((void *)buf, sector, num_sectors, true);
}

static bool _cow_read_cow(void *ctx, uint32_t cow_sector, uint32_t num_sectors, void *buf)
{
    (void)ctx;
    return _file_based_gpp_raw_rw(buf, g_cow_base_sector + cow_sector, num_sectors, false);
}

static bool _cow_write_cow(void *ctx, uint32_t cow_sector, uint32_t num_sectors, const void *buf)
{
    (void)ctx;
    return _file_based_gpp_raw_rw((void *)buf, g_cow_base_sector + cow_sector, num_sectors, true);
}

static bool _cow_sync_cow(void *ctx)
{
    (void)ctx;
    return true;
}

static void _sdmmc_ensure_device_attached(void)
{
    // This ensures that the sd device address space handle is always attached,
    // even if FS hasn't attached it
    static bool did_attach = false;
    if (!did_attach)
    {
        svcAttachDeviceAddressSpace(DeviceName_SDMMC1A, *sdmmc_das_handle);
        did_attach = true;
    }
}

static void _sdmmc_ensure_initialized(void)
{
    // First Initial init
    if (!sdmmc_first_init)
    {
        sdmmc_initialize();
        sdmmc_first_init = true;
    }
    else
    {
        // The boot sysmodule will eventually kill power to SD.
        // Detect this, and reinitialize when it happens.
        if (!init_done)
        {
            if (sdmmc_get_sd_power_enabled() == 0)
            {
                sdmmc_finalize();
                sdmmc_initialize();
                init_done = true;
            }
        }
    }
}

static void _file_based_update_filename(char *outFilename, unsigned int sd_path_len, unsigned int part_idx)
{
    snprintf(outFilename + sd_path_len, 3, "%02d", part_idx);
}

static void _file_based_emmc_finalize(void)
{
    if ((emuMMC_ctx.EMMC_Type == emuMMC_SD_File) && fat_mounted)
    {
        // Close all open handles.
        f_close(&f_emu.fp_boot0);
        f_close(&f_emu.fp_boot1);

        for (int i = 0; i < f_emu.parts; i++)
            f_close(&f_emu.fp_gpp[i]);

        // Force unmount FAT volume.
        f_mount(NULL, "", 1);

        fat_mounted = false;
    }
}

static void _nand_patrol_ensure_integrity(void)
{
    fs_nand_patrol_t nand_patrol;
    static bool nand_patrol_checked = false;

    if (!nand_patrol_checked)
    {
        if (emuMMC_ctx.EMMC_Type == emuMMC_SD_Raw)
        {
            unsigned int nand_patrol_sector = emuMMC_ctx.EMMC_StoragePartitionOffset + NAND_PATROL_SECTOR;
            if (!sdmmc_storage_read(&sd_storage, nand_patrol_sector, 1, &nand_patrol))
                goto out;

            // Clear nand patrol if last offset exceeds storage.
            if (nand_patrol.offset > sd_storage.sec_cnt)
            {
                memset(&nand_patrol, 0, sizeof(fs_nand_patrol_t));
                sdmmc_storage_write(&sd_storage, nand_patrol_sector, 1, &nand_patrol);
            }
        }
        else if (emuMMC_ctx.EMMC_Type == emuMMC_SD_File && fat_mounted)
        {
            FIL *fp = &f_emu.fp_boot0;
            if (f_lseek(fp, NAND_PATROL_OFFSET) != FR_OK)
                goto out;

            if (f_read_fast(fp, &nand_patrol, sizeof(fs_nand_patrol_t)) != FR_OK)
                goto out;

            // Clear nand patrol if last offset exceeds total file based size.
            if (nand_patrol.offset > f_emu.total_sect)
            {
                memset(&nand_patrol, 0, sizeof(fs_nand_patrol_t));

                if (f_lseek(fp, NAND_PATROL_OFFSET) != FR_OK)
                    goto out;

                if (f_write_fast(fp, &nand_patrol, sizeof(fs_nand_patrol_t)) != FR_OK)
                    goto out;

                f_sync(fp);
            }
        }

out:
        nand_patrol_checked = true;
    }
}

void sdmmc_finalize(void)
{
    if (!sdmmc_storage_end(&sd_storage))
        fatal_abort(Fatal_InitSD);

    storageSDinitialized = false;
}

static void _file_based_emmc_initialize(void)
{
    char path[sizeof(emuMMC_ctx.storagePath) + 0x20];
    memset(&path, 0, sizeof(path));

    memcpy(path, (void *)emuMMC_ctx.storagePath, sizeof(emuMMC_ctx.storagePath));
    strcat(path, "/eMMC/");
    int path_len = strlen(path);

    // Open BOOT0 physical partition.
    memcpy(path + path_len, "BOOT0", 6);
    if (f_open(&f_emu.fp_boot0, path, FA_READ | FA_WRITE) != FR_OK)
        fatal_abort(Fatal_FatfsFileOpen);
    if (!f_expand_cltbl(&f_emu.fp_boot0, EMUMMC_FP_CLMT_COUNT, f_emu.clmt_boot0, f_size(&f_emu.fp_boot0)))
        fatal_abort(Fatal_FatfsMemExhaustion);

    // Open BOOT1 physical partition.
    memcpy(path + path_len, "BOOT1", 6);
    if (f_open(&f_emu.fp_boot1, path, FA_READ | FA_WRITE) != FR_OK)
        fatal_abort(Fatal_FatfsFileOpen);
    if (!f_expand_cltbl(&f_emu.fp_boot1, EMUMMC_FP_CLMT_COUNT, f_emu.clmt_boot1, f_size(&f_emu.fp_boot1)))
        fatal_abort(Fatal_FatfsMemExhaustion);

    // Open handles for GPP physical partition files.
    _file_based_update_filename(path, path_len, 00);

    if (f_open(&f_emu.fp_gpp[0], path, FA_READ | FA_WRITE) != FR_OK)
        fatal_abort(Fatal_FatfsFileOpen);
    if (!f_expand_cltbl(&f_emu.fp_gpp[0], EMUMMC_FP_CLMT_COUNT, &f_emu.clmt_gpp[0], f_size(&f_emu.fp_gpp[0])))
        fatal_abort(Fatal_FatfsMemExhaustion);

    f_emu.part_size = (uint64_t)f_size(&f_emu.fp_gpp[0]) >> 9;
    f_emu.total_sect = f_emu.part_size;

    // Iterate folder for split parts and stop if next doesn't exist.
    for (f_emu.parts = 1; f_emu.parts < EMUMMC_FILE_MAX_PARTS; f_emu.parts++)
    {
        _file_based_update_filename(path, path_len, f_emu.parts);

        if (f_open(&f_emu.fp_gpp[f_emu.parts], path, FA_READ | FA_WRITE) != FR_OK)
        {
            // Check if single file.
            if (f_emu.parts == 1)
                f_emu.parts = 0;

            break;
        }

        if (!f_expand_cltbl(&f_emu.fp_gpp[f_emu.parts], EMUMMC_FP_CLMT_COUNT,
            &f_emu.clmt_gpp[f_emu.parts * EMUMMC_FP_CLMT_COUNT], f_size(&f_emu.fp_gpp[f_emu.parts])))
        {
            fatal_abort(Fatal_FatfsMemExhaustion);
        }

        f_emu.total_sect += (uint64_t)f_size(&f_emu.fp_gpp[f_emu.parts]) >> 9;
    }

    g_raw_file_total_sect = f_emu.total_sect;

    if (g_cow_engine.initialized)
    {
        // Preserve active in-RAM COW state across any mid-boot SD re-initialization
        f_emu.total_sect = g_cow_engine.hdr.gpp_total_sectors;
        return;
    }

    // Detect cowMMC v2 superblock at offset 32 MB (Slot 0 = sector 65536, Slot 1 = sector 65568) of /eMMC/00
    if (g_raw_file_total_sect > COW_DEFAULT_PREFIX_SECTS)
    {
        bool cow_magic_present = false;
        cow_header_t probe_hdr;

        if (_file_based_gpp_raw_rw(&probe_hdr, COW_DEFAULT_PREFIX_SECTS, 1U, false) &&
            probe_hdr.magic == COW_MAGIC)
        {
            cow_magic_present = true;
        }
        else if (g_raw_file_total_sect > COW_DEFAULT_PREFIX_SECTS + COW_BLOCK_SECTORS &&
                 _file_based_gpp_raw_rw(&probe_hdr, COW_DEFAULT_PREFIX_SECTS + COW_BLOCK_SECTORS, 1U, false) &&
                 probe_hdr.magic == COW_MAGIC)
        {
            cow_magic_present = true;
        }

        if (cow_magic_present)
        {
            g_cow_base_sector = COW_DEFAULT_PREFIX_SECTS;
            uint64_t raw_slots = (g_raw_file_total_sect - g_cow_base_sector) / COW_BLOCK_SECTORS;
            uint32_t avail_slots = (raw_slots > UINT32_MAX) ? UINT32_MAX : (uint32_t)raw_slots;

            if (avail_slots <= COW_SUPERBLOCK_SLOTS)
                fatal_abort(Fatal_FatfsFileOpen);

            cow_io_ops_t cow_io = {
                .user_ctx      = NULL,
                .read_base_gpp = _cow_read_base_gpp,
                .read_prefix   = _cow_read_prefix,
                .write_prefix  = _cow_write_prefix,
                .read_cow      = _cow_read_cow,
                .write_cow     = _cow_write_cow,
                .sync_cow      = _cow_sync_cow,
            };

            if (!cow_engine_init_bounded(&g_cow_engine, &cow_io, avail_slots, true))
                fatal_abort(Fatal_FatfsFileOpen);

            // Expose the full virtual GPP sector count to FS.kip & NAND Patrol
            f_emu.total_sect = g_cow_engine.hdr.gpp_total_sectors;
        }
    }
}

bool sdmmc_initialize(void)
{
    if (!storageSDinitialized)
    {
        int retries = 3;
        while (retries)
        {
            if (nx_sd_initialize(false))
            {
                storageSDinitialized = true;

                // Init file based emummc.
                if ((emuMMC_ctx.EMMC_Type == emuMMC_SD_File) && !fat_mounted)
                {
                    if (f_mount(&f_emu.sd_fs, "", 1) != FR_OK)
                        fatal_abort(Fatal_InitSD);
                    else
                        fat_mounted = true;

                    _file_based_emmc_initialize();
                }

                // Check if nand patrol offset is inside limits.
                _nand_patrol_ensure_integrity();

                break;
            }

            retries--;
        }

        if (!storageSDinitialized)
            fatal_abort(Fatal_InitSD);
    }

    return storageSDinitialized;
}

sdmmc_accessor_t *sdmmc_accessor_get(int mmc_id)
{
    sdmmc_accessor_t *_this;
    switch (mmc_id)
    {
    case FS_SDMMC_EMMC:
        _this = sdmmc_accessor_nand();
        break;
    case FS_SDMMC_SD:
        _this = sdmmc_accessor_sd();
        break;
    case FS_SDMMC_GC:
        _this = sdmmc_accessor_gc();
        break;
    default:
        fatal_abort(Fatal_InvalidAccessor);
    }

    return _this;
}

void mutex_lock_handler(int mmc_id)
{
    if (custom_driver)
        lock_mutex(sd_mutex);

    lock_mutex(nand_mutex);
}

void mutex_unlock_handler(int mmc_id)
{
    unlock_mutex(nand_mutex);

    if (custom_driver)
        unlock_mutex(sd_mutex);
}

int sdmmc_nand_get_active_partition_index()
{
    switch (*active_partition)
    {
    case FS_EMMC_PARTITION_GPP:
        return 2;
    case FS_EMMC_PARTITION_BOOT1:
        return 1;
    case FS_EMMC_PARTITION_BOOT0:
        return 0;
    }

    fatal_abort(Fatal_InvalidAccessor);
}

static uint64_t emummc_read_write_inner(void *buf, unsigned int sector, unsigned int num_sectors, bool is_write)
{
    if (emuMMC_ctx.EMMC_Type == emuMMC_SD_Raw)
    {
        // raw partition sector offset: emuMMC_ctx.EMMC_StoragePartitionOffset.
        sector += emuMMC_ctx.EMMC_StoragePartitionOffset;
        // Set physical partition offset.
        sector += (sdmmc_nand_get_active_partition_index() * BOOT_PARTITION_SIZE);

        if (__builtin_expect(sector + num_sectors > sd_storage.sec_cnt, 0))
            return 0; // Out of bounds. Can only happen with Nand Patrol if resized.

        if (!is_write)
            return sdmmc_storage_read(&sd_storage, sector, num_sectors, buf);
        else
            return sdmmc_storage_write(&sd_storage, sector, num_sectors, buf);
    }

    // File based emummc.
    FIL *fp = NULL;
    switch (*active_partition)
    {
    case FS_EMMC_PARTITION_GPP:
        if (g_cow_engine.initialized)
        {
            if (!is_write)
                return cow_engine_read_gpp(&g_cow_engine, sector, num_sectors, buf);
            else
                return cow_engine_write_gpp(&g_cow_engine, sector, num_sectors, buf);
        }
        return _file_based_gpp_raw_rw(buf, sector, num_sectors, is_write);

    case FS_EMMC_PARTITION_BOOT1:
        fp = &f_emu.fp_boot1;
        break;

    case FS_EMMC_PARTITION_BOOT0:
        fp = &f_emu.fp_boot0;
        break;
    }

    if (f_lseek(fp, (uint64_t)sector << 9) != FR_OK)
        return 0; // Out of bounds. Can only happen with Nand Patrol if resized.

    if (!is_write)
        return !f_read_fast(fp, buf, (uint64_t)num_sectors << 9);
    else
        return !f_write_fast(fp, buf, (uint64_t)num_sectors << 9);
}

// Controller open wrapper
uint64_t sdmmc_wrapper_controller_open(int mmc_id)
{
    uint64_t result;
    sdmmc_accessor_t *_this;
    _this = sdmmc_accessor_get(mmc_id);

    if (_this != NULL)
    {
        // Lock eMMC xfer while SD card is being initialized by FS.
        if (mmc_id == FS_SDMMC_SD)
            mutex_lock_handler(FS_SDMMC_EMMC); // Recursive Mutex, handler will lock SD as well if custom_driver

        result = _this->vtab->sdmmc_accessor_controller_open(_this);

        if (mmc_id == FS_SDMMC_EMMC && result == 0)
            _cow_apply_capacity_override(_this);

        // Unlock eMMC.
        if (mmc_id == FS_SDMMC_SD)
            mutex_unlock_handler(FS_SDMMC_EMMC);

        return result;
    }

    fatal_abort(Fatal_OpenAccessor);
}

// Controller close wrapper
uint64_t sdmmc_wrapper_controller_close(int mmc_id)
{
    sdmmc_accessor_t *_this;
    _this = sdmmc_accessor_get(mmc_id);

    if (_this != NULL)
    {
        if (mmc_id == FS_SDMMC_SD)
            return 0;

        if (mmc_id == FS_SDMMC_EMMC)
        {
            // Close file handles and unmount
            _file_based_emmc_finalize();

            // Close SD
            sdmmc_accessor_get(FS_SDMMC_SD)->vtab->sdmmc_accessor_controller_close(sdmmc_accessor_get(FS_SDMMC_SD));

            // Close eMMC
            return _this->vtab->sdmmc_accessor_controller_close(_this);
        }

        return _this->vtab->sdmmc_accessor_controller_close(_this);
    }

    fatal_abort(Fatal_CloseAccessor);
}

// FS read wrapper.
uint64_t sdmmc_wrapper_read(void *buf, uint64_t bufSize, int mmc_id, unsigned int sector, unsigned int num_sectors)
{
    sdmmc_accessor_t *_this;
    uint64_t read_res;

    _this = sdmmc_accessor_get(mmc_id);

    if (_this != NULL)
    {
        if (mmc_id == FS_SDMMC_EMMC || mmc_id == FS_SDMMC_SD)
        {
            mutex_lock_handler(mmc_id);
            // Assign FS accessor to the SDMMC driver
            _current_accessor = _this;
            // Make sure we're attached to the device address space.
            _sdmmc_ensure_device_attached();
            // Make sure we're still initialized if boot killed sd card power.
            _sdmmc_ensure_initialized();
        }

        if (mmc_id == FS_SDMMC_EMMC)
        {
            _cow_apply_capacity_override(_this);

            // Call hekates driver.
            if (emummc_read_write_inner(buf, sector, num_sectors, false))
            {
                mutex_unlock_handler(mmc_id);
                return 0;
            }

            mutex_unlock_handler(mmc_id);
            return FS_READ_WRITE_ERROR;
        }

        if (mmc_id == FS_SDMMC_SD)
        {
            static bool first_sd_read = true;
            if (first_sd_read)
            {
                first_sd_read = false;
                if (emuMMC_ctx.EMMC_Type == emuMMC_SD_Raw)
                {
                    // Because some SD cards have issues with emuMMC's driver
                    // we currently swap to FS's driver after first SD read
                    // for raw based emuMMC
                    custom_driver = false;
                    // FS will handle sd mutex w/o custom driver from here on
                    unlock_mutex(sd_mutex);
                }
            }

            // Call hekate's driver.
            if (sdmmc_storage_read(&sd_storage, sector, num_sectors, buf))
            {
                mutex_unlock_handler(mmc_id);
                return 0;
            }

            mutex_unlock_handler(mmc_id);
            return FS_READ_WRITE_ERROR;
        }

        read_res = _this->vtab->read_write(_this, sector, num_sectors, buf, bufSize, 1);
        return read_res;
    }

    fatal_abort(Fatal_ReadNoAccessor);
}

// FS write wrapper.
uint64_t sdmmc_wrapper_write(int mmc_id, unsigned int sector, unsigned int num_sectors, void *buf, uint64_t bufSize)
{
    sdmmc_accessor_t *_this;
    uint64_t write_res;

    _this = sdmmc_accessor_get(mmc_id);

    if (_this != NULL)
    {
        if (mmc_id == FS_SDMMC_EMMC)
        {
            mutex_lock_handler(mmc_id);
            _current_accessor = _this;
            _cow_apply_capacity_override(_this);

            // Call hekates driver.
            if (emummc_read_write_inner(buf, sector, num_sectors, true))
            {
                mutex_unlock_handler(mmc_id);
                return 0;
            }

            mutex_unlock_handler(mmc_id);
            return FS_READ_WRITE_ERROR;
        }

        if (mmc_id == FS_SDMMC_SD)
        {
            mutex_lock_handler(mmc_id);
            _current_accessor = _this;

            // Call hekates driver.
            if (sdmmc_storage_write(&sd_storage, sector, num_sectors, buf))
            {
                mutex_unlock_handler(mmc_id);
                return 0;
            }

            mutex_unlock_handler(mmc_id);
            return FS_READ_WRITE_ERROR;
        }

        write_res = _this->vtab->read_write(_this, sector, num_sectors, buf, bufSize, 0);
        return write_res;
    }

    fatal_abort(Fatal_WriteNoAccessor);
}
