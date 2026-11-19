/*
 * Copyright (c) Atmosphère-NX
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
#include <exosphere.hpp>
#include "fusee_emummc.hpp"
#include "fusee_mmc.hpp"
#include "fusee_sd_card.hpp"
#include "fusee_fatal.hpp"
#include "fusee_malloc.hpp"
#include "fs/fusee_fs_api.hpp"
#include "fs/fusee_fs_storage.hpp"
#include "fatfs/ff.h"

namespace ams::nxboot {

    namespace {

        class SdCardStorage : public fs::IStorage {
            public:
                virtual Result Read(s64 offset, void *buffer, size_t size) override {
                    if (!util::IsAligned(offset, sdmmc::SectorSize) || !util::IsAligned(size, sdmmc::SectorSize)) {
                        ShowFatalError("SdCard: unaligned access to %" PRIx64 ", size=%" PRIx64"\n", static_cast<u64>(offset), static_cast<u64>(size));
                    }

                    R_RETURN(ReadSdCard(buffer, size, offset / sdmmc::SectorSize, size / sdmmc::SectorSize));
                }

                virtual Result Flush() override {
                    R_SUCCEED();
                }

                virtual Result GetSize(s64 *out) override {
                    u32 num_sectors;
                    R_TRY(GetSdCardMemoryCapacity(std::addressof(num_sectors)));

                    *out = static_cast<s64>(num_sectors) * static_cast<s64>(sdmmc::SectorSize);
                    R_SUCCEED();
                }

                virtual Result Write(s64 offset, const void *buffer, size_t size) override {
                    R_THROW(fs::ResultUnsupportedOperation());
                }

                virtual Result SetSize(s64 size) override {
                    R_THROW(fs::ResultUnsupportedOperation());
                }
        };

        template<sdmmc::MmcPartition Partition>
        class MmcPartitionStorage : public fs::IStorage {
            public:
                constexpr MmcPartitionStorage() { /* ... */ }

                virtual Result Read(s64 offset, void *buffer, size_t size) override {
                    if (!util::IsAligned(offset, sdmmc::SectorSize) || !util::IsAligned(size, sdmmc::SectorSize)) {
                        ShowFatalError("SdCard: unaligned access to %" PRIx64 ", size=%" PRIx64"\n", static_cast<u64>(offset), static_cast<u64>(size));
                    }

                    R_RETURN(ReadMmc(buffer, size, Partition, offset / sdmmc::SectorSize, size / sdmmc::SectorSize));
                }

                virtual Result Flush() override {
                    R_SUCCEED();
                }

                virtual Result GetSize(s64 *out) override {
                    u32 num_sectors;
                    R_TRY(GetMmcMemoryCapacity(std::addressof(num_sectors), Partition));

                    *out = num_sectors * sdmmc::SectorSize;
                    R_SUCCEED();
                }

                virtual Result Write(s64 offset, const void *buffer, size_t size) override {
                    R_THROW(fs::ResultUnsupportedOperation());
                }

                virtual Result SetSize(s64 size) override {
                    R_THROW(fs::ResultUnsupportedOperation());
                }
        };

        using MmcBoot0Storage = MmcPartitionStorage<sdmmc::MmcPartition_BootPartition1>;
        using MmcBoot1Storage = MmcPartitionStorage<sdmmc::MmcPartition_BootPartition2>;
        using MmcUserStorage  = MmcPartitionStorage<sdmmc::MmcPartition_UserData>;

        constinit char g_emummc_path[0x300];

        class EmummcFileStorage : public fs::IStorage {
            private:
                s64 m_file_size;
                fs::FileHandle m_handles[64];
                bool m_open[64];
                int m_file_path_ofs;
            private:
                void EnsureFile(int id) {
                    if (!m_open[id]) {
                        /* Update path. */
                        g_emummc_path[m_file_path_ofs + 1] = '0' + (id % 10);
                        g_emummc_path[m_file_path_ofs + 0] = '0' + (id / 10);

                        /* Open new file. */
                        const Result result = fs::OpenFile(m_handles + id, g_emummc_path, fs::OpenMode_Read);
                        if (R_FAILED(result)) {
                            ShowFatalError("Failed to open emummc user %02d file: 0x%08" PRIx32 "!\n", id, result.GetValue());
                        }

                        m_open[id] = true;
                    }
                }
            public:
                EmummcFileStorage(fs::FileHandle user00, int ofs) : m_file_path_ofs(ofs) {
                    const Result result = fs::GetFileSize(std::addressof(m_file_size), user00);
                    if (R_FAILED(result)) {
                        ShowFatalError("Failed to get emummc file size: 0x%08" PRIx32 "!\n", result.GetValue());
                    }

                    for (size_t i = 0; i < util::size(m_handles); ++i) {
                        m_open[i] = false;
                    }

                    m_handles[0] = user00;
                    m_open[0]    = true;
                }

                virtual Result Read(s64 offset, void *buffer, size_t size) override {
                    int file   = offset / m_file_size;
                    s64 subofs = offset % m_file_size;

                    u8 *cur_dst = static_cast<u8 *>(buffer);

                    for (/* ... */; size > 0; ++file) {
                        /* Ensure the current file is open. */
                        EnsureFile(file);

                        /* Perform the current read. */
                        const size_t cur_size = std::min<size_t>(m_file_size - subofs, size);
                        R_TRY(fs::ReadFile(m_handles[file], subofs, cur_dst, cur_size));

                        /* Advance. */
                        cur_dst += cur_size;
                        size -= cur_size;
                        subofs = 0;
                    }

                    R_SUCCEED();
                }

                virtual Result Flush() override {
                    R_THROW(fs::ResultUnsupportedOperation());
                }

                virtual Result GetSize(s64 *out) override {
                    R_THROW(fs::ResultUnsupportedOperation());
                }

                virtual Result Write(s64 offset, const void *buffer, size_t size) override {
                    R_THROW(fs::ResultUnsupportedOperation());
                }

                virtual Result SetSize(s64 size) override {
                    R_THROW(fs::ResultUnsupportedOperation());
                }
        };

        constinit SdCardStorage g_sd_card_storage;

        constinit MmcBoot0Storage g_mmc_boot0_storage;
        constinit MmcUserStorage g_mmc_user_storage;

        constinit fs::IStorage *g_boot0_storage = nullptr;
        constinit fs::IStorage *g_user_storage  = nullptr;

        constinit fs::SubStorage *g_package2_storage = nullptr;

        struct Guid {
            u32 data1;
            u16 data2;
            u16 data3;
            u8 data4[8];
        };
        static_assert(sizeof(Guid) == 0x10);

        struct GptHeader {
            char signature[8];
            u32 revision;
            u32 header_size;
            u32 header_crc32;
            u32 reserved0;
            u64 my_lba;
            u64 alt_lba;
            u64 first_usable_lba;
            u64 last_usable_lba;
            Guid disk_guid;
            u64 partition_entry_lba;
            u32 number_of_partition_entries;
            u32 size_of_partition_entry;
            u32 partition_entry_array_crc32;
            u32 reserved1;
        };
        static_assert(sizeof(GptHeader) == 0x60);

        struct GptPartitionEntry {
            Guid partition_type_guid;
            Guid unique_partition_guid;
            u64 starting_lba;
            u64 ending_lba;
            u64 attributes;
            char partition_name[0x48];
        };
        static_assert(sizeof(GptPartitionEntry) == 0x80);

        struct Gpt {
            GptHeader header;
            u8 padding[0x1A0];
            GptPartitionEntry entries[128];
        };
        static_assert(sizeof(Gpt) == 16_KB + 0x200);

        constexpr const u16 Package2PartitionName[] = {
            'B', 'C', 'P', 'K', 'G', '2', '-', '1', '-', 'N', 'o', 'r', 'm', 'a', 'l', '-', 'M', 'a', 'i', 'n', 0
        };

        constinit MmcBoot1Storage g_mmc_boot1_storage;

        namespace cow {

            constexpr inline u32 Magic                  = 0x30574F43U; /* "COW0" */
            constexpr inline u32 VersionV2              = 2U;
            constexpr inline u32 Version                = VersionV2;
            constexpr inline u32 SectorSize             = 512U;
            constexpr inline u32 BlockSectors           = 32U;
            constexpr inline u32 BlockShift             = 5U;
            constexpr inline u32 BlockBytes             = 16384U;
            constexpr inline u32 MinL1Entries           = 1024U;
            constexpr inline u32 MaxL1Entries           = 8192U;
            constexpr inline u32 V2DefaultL1Entries     = 4096U;
            constexpr inline u32 L2Shift                = 12U;
            constexpr inline u32 L2Entries              = (1U << L2Shift);
            constexpr inline u32 SuperblockSlots        = 2U;
            constexpr inline u32 FirstDataSlot          = 4U;
            constexpr inline u32 PrefixSectors          = 65536U; /* 32 MiB */
            constexpr inline size_t PrefixBytes         = static_cast<size_t>(PrefixSectors) * SectorSize;
            constexpr inline u32 FlagEphemeral          = (1U << 0);
            constexpr inline u32 FlagThinProvision      = (1U << 1);
            constexpr inline u32 FlagNeedsRebase        = (1U << 2);
            constexpr inline u32 FlagFwMismatch         = (1U << 3);
            constexpr inline u32 FlagConflictLocked     = (1U << 4);
            constexpr inline u32 AllocThick             = 0U;
            constexpr inline u32 AllocThin              = 1U;
            constexpr inline u32 DriftBoot0Pkg1         = (1U << 0);
            constexpr inline u32 DriftBoot0Patrol       = (1U << 1);
            constexpr inline u32 DriftBoot1             = (1U << 2);
            constexpr inline u32 DriftGpt               = (1U << 3);
            constexpr inline u32 DriftBcpkg2            = (1U << 4);
            constexpr inline u32 DefaultSdReserveMb     = 512U;
            constexpr inline u32 DefaultWatermarkPct    = 80U;
            constexpr inline u32 DefaultGrowStepMb      = 256U;
            constexpr inline u32 DefaultPartMaxMb       = 4092U;
            constexpr inline u32 MaxSplitParts          = 64U;
            constexpr inline size_t BootPartBytes       = 4U * 1024U * 1024U;
            constexpr inline size_t Boot0BctSaveOffset  = 0xFC000U;
            constexpr inline size_t Boot0BctSaveSize    = 0x4000U;
            constexpr inline size_t Boot0PatrolOffset   = 0x184000U;
            constexpr inline size_t Boot0PatrolSize     = 0x200U;

            constexpr u32 L1SlotsFor(u32 n) {
                return (((n * sizeof(u32)) + BlockBytes - 1U) / BlockBytes);
            }

            struct CowHeader {
                u32  magic;
                u32  version;
                u64  seq_num;
                u64  gpp_total_sectors;
                u32  prefix_sectors;
                u32  total_slots;
                u32  next_free_slot;
                u32  flags;
                u32  dirty_data_blocks;
                u32  dirty_l2_pages;
                u32  l1_crc32;
                u32  header_ext_size;
                u64  physical_base_sectors;
                u32  max_overlay_slots;
                u32  l1_entries;
                u32  l1_slot_a;
                u32  l1_slot_b;
                u32  first_data_slot;
                u32  thin_grow_step_slots;
                u16  thin_high_watermark_pct;
                u8   alloc_policy;
                u8   block_shift;
                u8   l2_shift;
                u8   reserved_geom[3];
                u32  part_max_sectors;
                u32  base_hos_version;
                u64  base_gen_id;
                u8   overlay_uuid[16];
                u8   parent_snapshot_uuid[16];
                u32  base_pkg1_ver;
                u32  ext_meta_slot;
                u8   emmc_cid_sha256_16[16];
                u8   boot0_pkg1_sha256[32];
                u8   boot1_sha256[32];
                u8   gpt_sha256[32];
                u8   bcpkg2_sha256[32];
                u8   system_digest_sha256[32];
                u8   user_digest_sha256[32];
                char overlay_name[32];
                u64  created_epoch;
                u64  last_rebase_seq;
                u64  last_verified_base_gen_id;
                u32  snapshot_count;
                u32  conflict_count;
                u32  base_drift_part_mask;
                u32  reserved_meta;
                u8   boot0_patrol_sha256_16[16];
                u8   last_snapshot_sha256_16[16];
                u8   reserved_v2[44];
                u32  header_crc32;
            };
            static_assert(sizeof(CowHeader) == SectorSize);
            static_assert(offsetof(CowHeader, header_ext_size) == 0x034);
            static_assert(offsetof(CowHeader, physical_base_sectors) == 0x038);
            static_assert(offsetof(CowHeader, header_crc32) == 0x1FC);

            struct AutoInitConfig {
                u32  version;
                u32  size_mb;
                bool size_auto;
                u32  max_size_mb;
                bool max_size_auto;
                u32  sd_reserve_mb;
                u32  virtual_gb;
                u32  thin;
                u32  grow_step_mb;
                u32  watermark_pct;
                u32  part_max_mb;
                u32  ephemeral;
                u32  strict_patrol;
                bool invalid;
            };

            struct BaseDigests {
                u8   boot0_pkg1_sha256[32];
                u8   boot0_patrol_sha256_16[16];
                u8   boot1_sha256[32];
                u8   gpt_sha256[32];
                u8   bcpkg2_sha256[32];
                u64  phys_gpp_sectors;
                bool gpt_valid;
                bool pkg2_valid;
            };

            class Sha256Context {
                private:
                    u32 m_state[8];
                    u64 m_bitlen;
                    u8  m_buf[64];
                    size_t m_buflen;

                    static constexpr u32 Ror(u32 v, u32 s) {
                        return (v >> s) | (v << (32U - s));
                    }

                    void Transform(const u8 block[64]) {
                        static constexpr u32 K[64] = {
                            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
                            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
                            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
                            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
                            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
                            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
                            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
                            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
                        };
                        u32 w[64];
                        for (size_t i = 0; i < 16; ++i) {
                            w[i] = (static_cast<u32>(block[i * 4 + 0]) << 24) |
                                   (static_cast<u32>(block[i * 4 + 1]) << 16) |
                                   (static_cast<u32>(block[i * 4 + 2]) << 8)  |
                                   (static_cast<u32>(block[i * 4 + 3]));
                        }
                        for (size_t i = 16; i < 64; ++i) {
                            const u32 s0 = Ror(w[i - 15], 7U) ^ Ror(w[i - 15], 18U) ^ (w[i - 15] >> 3U);
                            const u32 s1 = Ror(w[i - 2], 17U) ^ Ror(w[i - 2], 19U)  ^ (w[i - 2] >> 10U);
                            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                        }
                        u32 a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
                        u32 e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
                        for (size_t i = 0; i < 64; ++i) {
                            const u32 S1 = Ror(e, 6U) ^ Ror(e, 11U) ^ Ror(e, 25U);
                            const u32 ch = (e & f) ^ ((~e) & g);
                            const u32 temp1 = h + S1 + ch + K[i] + w[i];
                            const u32 S0 = Ror(a, 2U) ^ Ror(a, 13U) ^ Ror(a, 22U);
                            const u32 maj = (a & b) ^ (a & c) ^ (b & c);
                            const u32 temp2 = S0 + maj;
                            h = g; g = f; f = e; e = d + temp1;
                            d = c; c = b; b = a; a = temp1 + temp2;
                        }
                        m_state[0] += a; m_state[1] += b; m_state[2] += c; m_state[3] += d;
                        m_state[4] += e; m_state[5] += f; m_state[6] += g; m_state[7] += h;
                    }
                public:
                    void Init() {
                        m_state[0] = 0x6a09e667U; m_state[1] = 0xbb67ae85U;
                        m_state[2] = 0x3c6ef372U; m_state[3] = 0xa54ff53aU;
                        m_state[4] = 0x510e527fU; m_state[5] = 0x9b05688cU;
                        m_state[6] = 0x1f83d9abU; m_state[7] = 0x5be0cd19U;
                        m_bitlen = 0;
                        m_buflen = 0;
                    }

                    void Update(const void *data, size_t len) {
                        const u8 *p = static_cast<const u8 *>(data);
                        m_bitlen += static_cast<u64>(len) * 8ULL;
                        while (len > 0) {
                            const size_t take = std::min(len, sizeof(m_buf) - m_buflen);
                            std::memcpy(m_buf + m_buflen, p, take);
                            m_buflen += take;
                            p += take;
                            len -= take;
                            if (m_buflen == sizeof(m_buf)) {
                                Transform(m_buf);
                                m_buflen = 0;
                            }
                        }
                    }

                    void Final(u8 out[32]) {
                        m_buf[m_buflen++] = 0x80U;
                        if (m_buflen > 56U) {
                            std::memset(m_buf + m_buflen, 0, 64U - m_buflen);
                            Transform(m_buf);
                            m_buflen = 0;
                        }
                        std::memset(m_buf + m_buflen, 0, 56U - m_buflen);
                        for (int i = 0; i < 8; ++i) {
                            m_buf[56 + i] = static_cast<u8>((m_bitlen >> ((7 - i) * 8)) & 0xFFU);
                        }
                        Transform(m_buf);
                        for (size_t i = 0; i < 8; ++i) {
                            out[i * 4 + 0] = static_cast<u8>((m_state[i] >> 24) & 0xFFU);
                            out[i * 4 + 1] = static_cast<u8>((m_state[i] >> 16) & 0xFFU);
                            out[i * 4 + 2] = static_cast<u8>((m_state[i] >> 8)  & 0xFFU);
                            out[i * 4 + 3] = static_cast<u8>(m_state[i] & 0xFFU);
                        }
                    }
            };

            u32 CalculateCrc32(const void *data, size_t len) {
                const u8 *p = static_cast<const u8 *>(data);
                u32 crc = 0xFFFFFFFFU;
                for (size_t i = 0; i < len; ++i) {
                    crc ^= p[i];
                    for (int b = 0; b < 8; ++b) {
                        crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
                    }
                }
                return ~crc;
            }

            u32 CalculateZeroCrc32(size_t byte_len) {
                u32 crc = 0xFFFFFFFFU;
                for (size_t i = 0; i < byte_len; ++i) {
                    for (int b = 0; b < 8; ++b) {
                        crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
                    }
                }
                return ~crc;
            }

            bool IsZero(const u8 *buf, size_t len) {
                for (size_t i = 0; i < len; ++i) {
                    if (buf[i] != 0U) return false;
                }
                return true;
            }

            char *TrimWs(char *s) {
                while (*s == ' ' || *s == '\t') ++s;
                size_t n = std::strlen(s);
                while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
                    s[--n] = '\0';
                }
                return s;
            }

            bool StrEqCi(const char *a, const char *b) {
                while (*a && *b) {
                    const char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
                    const char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
                    if (ca != cb) return false;
                    ++a; ++b;
                }
                return *a == '\0' && *b == '\0';
            }

            bool ParseDecimalU32(const char *s, u32 *out) {
                if (s == nullptr || *s == '\0') return false;
                u64 acc = 0;
                for (const char *p = s; *p != '\0'; ++p) {
                    if (*p < '0' || *p > '9') return false;
                    acc = acc * 10ULL + static_cast<u64>(*p - '0');
                    if (acc > 0xFFFFFFFFULL) return false;
                }
                *out = static_cast<u32>(acc);
                return true;
            }

            void ExtractOverlayName(const char *base_path, size_t base_len, char *out, size_t out_sz) {
                if (out == nullptr || out_sz == 0) return;
                std::memset(out, 0, out_sz);
                if (base_path == nullptr || base_len == 0) {
                    std::strncpy(out, "COW0", out_sz - 1);
                    return;
                }
                size_t start = 0;
                for (size_t i = base_len; i > 0; --i) {
                    if (base_path[i - 1] == '/') {
                        start = i;
                        break;
                    }
                }
                const size_t name_len = (start < base_len) ? std::min(base_len - start, out_sz - 1) : std::min(base_len, out_sz - 1);
                if (start < base_len) {
                    std::memcpy(out, base_path + start, name_len);
                } else {
                    std::memcpy(out, base_path, name_len);
                }
            }

            void FillSuperblockV2(u8 *sb_buf, const CowHeader &tmpl, const u32 *l1_table, u64 seq_num) {
                std::memset(sb_buf, 0, BlockBytes);
                auto *hdr = reinterpret_cast<CowHeader *>(sb_buf);
                std::memcpy(hdr, std::addressof(tmpl), sizeof(*hdr));

                hdr->magic           = Magic;
                hdr->version         = VersionV2;
                hdr->seq_num         = seq_num;
                hdr->header_ext_size = SectorSize;
                hdr->block_shift     = BlockShift;
                hdr->l2_shift        = L2Shift;
                if (hdr->prefix_sectors == 0U) {
                    hdr->prefix_sectors = PrefixSectors;
                }
                if (hdr->physical_base_sectors == 0ULL) {
                    hdr->physical_base_sectors = hdr->gpp_total_sectors;
                }
                if (hdr->l1_entries == 0U) {
                    constexpr u64 SectorsPerL1 = static_cast<u64>(L2Entries) * BlockSectors;
                    const u64 needed_l1 = (hdr->gpp_total_sectors + SectorsPerL1 - 1ULL) / SectorsPerL1;
                    hdr->l1_entries = (needed_l1 <= V2DefaultL1Entries) ? V2DefaultL1Entries : MaxL1Entries;
                }
                const u32 l1_slots = L1SlotsFor(hdr->l1_entries);
                if (hdr->l1_slot_a == 0U && hdr->l1_slot_b == 0U && hdr->first_data_slot == 0U) {
                    hdr->l1_slot_a       = SuperblockSlots;
                    hdr->l1_slot_b       = SuperblockSlots + l1_slots;
                    hdr->first_data_slot = SuperblockSlots + 2U * l1_slots;
                }
                if (hdr->next_free_slot == 0U) {
                    hdr->next_free_slot = hdr->first_data_slot;
                }
                if (hdr->max_overlay_slots == 0U) {
                    hdr->max_overlay_slots = hdr->total_slots;
                }
                if (hdr->part_max_sectors == 0U) {
                    hdr->part_max_sectors = DefaultPartMaxMb * 2048U;
                }
                if (hdr->alloc_policy == AllocThin) {
                    hdr->flags |= FlagThinProvision;
                }

                const size_t l1_bytes = static_cast<size_t>(hdr->l1_entries) * sizeof(u32);
                hdr->l1_crc32     = (l1_table != nullptr) ? CalculateCrc32(l1_table, l1_bytes) : CalculateZeroCrc32(l1_bytes);
                hdr->header_crc32 = CalculateCrc32(sb_buf, offsetof(CowHeader, header_crc32));
            }

            constinit bool g_mmc_initialized = false;
            constinit u8  *g_chunk_buf = nullptr;
            constinit u8  *g_slot_buf  = nullptr;
            constinit u32 *g_l1_buf    = nullptr;
            constinit Gpt *g_gpt_buf   = nullptr;

            u8 *GetChunkBuffer() {
                if (g_chunk_buf == nullptr) {
                    g_chunk_buf = static_cast<u8 *>(AllocateAligned(512U * 1024U, 0x200));
                }
                return g_chunk_buf;
            }

            u8 *GetSlotBuffer() {
                if (g_slot_buf == nullptr) {
                    g_slot_buf = static_cast<u8 *>(AllocateAligned(BlockBytes, 0x200));
                }
                return g_slot_buf;
            }

            u32 *GetL1Buffer() {
                if (g_l1_buf == nullptr) {
                    g_l1_buf = static_cast<u32 *>(AllocateAligned(MaxL1Entries * sizeof(u32), 0x10));
                }
                return g_l1_buf;
            }

            Gpt *GetGptBuffer() {
                if (g_gpt_buf == nullptr) {
                    g_gpt_buf = static_cast<Gpt *>(AllocateAligned(sizeof(Gpt), 0x200));
                }
                return g_gpt_buf;
            }

            void EnsureMmcInitialized() {
                if (!g_mmc_initialized) {
                    if (const Result rc = InitializeMmc(); R_FAILED(rc)) {
                        ShowFatalError("COW: failed to initialize mmc: 0x%08" PRIx32 "\n", rc.GetValue());
                    }
                    g_mmc_initialized = true;
                }
            }

            bool PathExists(const char *path) {
                fs::DirectoryEntryType et;
                bool arch;
                return R_SUCCEEDED(fs::GetEntryType(std::addressof(et), std::addressof(arch), path));
            }

            Result EnsureDirectoryHierarchy(const char *path) {
                char buf[0x300];
                const size_t n = std::strlen(path);
                if (n == 0 || n >= sizeof(buf)) {
                    R_RETURN(fs::ResultInvalidArgument());
                }
                std::memcpy(buf, path, n + 1);
                for (size_t i = 1; i < n; ++i) {
                    if (buf[i] == '/') {
                        buf[i] = '\0';
                        const Result r = fs::CreateDirectory(buf);
                        if (R_FAILED(r) && !fs::ResultPathAlreadyExists::Includes(r)) {
                            R_RETURN(r);
                        }
                        buf[i] = '/';
                    }
                }
                const Result r = fs::CreateDirectory(buf);
                if (R_FAILED(r) && !fs::ResultPathAlreadyExists::Includes(r)) {
                    R_RETURN(r);
                }
                R_SUCCEED();
            }

            bool GetSdFreeBytes(u64 *out_free_bytes) {
                DWORD fre_clust = 0;
                FATFS *fs_obj = nullptr;
                if (::f_getfree("sdmc:", std::addressof(fre_clust), std::addressof(fs_obj)) != FR_OK ||
                    fs_obj == nullptr || fs_obj->csize == 0) {
                    return false;
                }
                *out_free_bytes = static_cast<u64>(fre_clust) * static_cast<u64>(fs_obj->csize) * static_cast<u64>(SectorSize);
                return true;
            }

            void ParseAutoInitConfig(const char *cfg_path, AutoInitConfig *cfg) {
                std::memset(cfg, 0, sizeof(*cfg));
                cfg->version       = VersionV2;
                cfg->size_mb       = 2048U;
                cfg->sd_reserve_mb = DefaultSdReserveMb;
                cfg->watermark_pct = DefaultWatermarkPct;
                cfg->part_max_mb   = DefaultPartMaxMb;

                fs::FileHandle fh;
                if (R_FAILED(fs::OpenFile(std::addressof(fh), cfg_path, fs::OpenMode_Read))) {
                    return;
                }
                ON_SCOPE_EXIT { fs::CloseFile(fh); };

                char buf[512];
                size_t br = 0;
                if (R_FAILED(fs::ReadFile(std::addressof(br), fh, 0, buf, sizeof(buf) - 1))) {
                    return;
                }
                buf[br] = '\0';

                bool has_size_mb      = false;
                bool has_max_size_mb  = false;
                bool has_grow_step_mb = false;
                bool has_v2_key       = false;

                char *p = buf;
                while (*p) {
                    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
                    if (!*p) break;
                    char *eol = p;
                    while (*eol && *eol != '\r' && *eol != '\n') ++eol;
                    if (*eol) *eol++ = '\0';
                    if (*p == '#' || *p == ';') {
                        p = eol;
                        continue;
                    }
                    if (char *eq = std::strchr(p, '='); eq != nullptr) {
                        *eq++ = '\0';
                        const char *key = TrimWs(p);
                        const char *val = TrimWs(eq);
                        u32 v = 0;

                        if (std::strcmp(key, "version") == 0) {
                            if (ParseDecimalU32(val, &v) && v == VersionV2) {
                                cfg->version = v;
                            } else {
                                cfg->invalid = true;
                            }
                        } else if (std::strcmp(key, "size_mb") == 0) {
                            if (StrEqCi(val, "auto")) {
                                cfg->size_auto = true;
                                has_size_mb = true;
                            } else if (ParseDecimalU32(val, &v) && v >= 16U) {
                                cfg->size_mb = v;
                                cfg->size_auto = false;
                                has_size_mb = true;
                            } else {
                                cfg->invalid = true;
                            }
                        } else if (std::strcmp(key, "max_size_mb") == 0) {
                            if (StrEqCi(val, "auto")) {
                                cfg->max_size_auto = true;
                                has_max_size_mb = true;
                                has_v2_key = true;
                            } else if (ParseDecimalU32(val, &v) && v >= 16U) {
                                cfg->max_size_mb = v;
                                cfg->max_size_auto = false;
                                has_max_size_mb = true;
                                has_v2_key = true;
                            } else {
                                cfg->invalid = true;
                            }
                        } else if (std::strcmp(key, "sd_reserve_mb") == 0) {
                            if (ParseDecimalU32(val, &v)) {
                                cfg->sd_reserve_mb = v;
                            } else {
                                cfg->invalid = true;
                            }
                        } else if (std::strcmp(key, "virtual_gb") == 0) {
                            if (ParseDecimalU32(val, &v) && v <= 512U) {
                                cfg->virtual_gb = v;
                                if (v > 0U) has_v2_key = true;
                            } else {
                                cfg->invalid = true;
                            }
                        } else if (std::strcmp(key, "thin") == 0) {
                            if (ParseDecimalU32(val, &v)) {
                                cfg->thin = (v != 0U) ? 1U : 0U;
                                if (cfg->thin != 0U) has_v2_key = true;
                            } else {
                                cfg->invalid = true;
                            }
                        } else if (std::strcmp(key, "grow_step_mb") == 0) {
                            if (ParseDecimalU32(val, &v) && v >= 1U) {
                                cfg->grow_step_mb = v;
                                has_grow_step_mb = true;
                                has_v2_key = true;
                            } else {
                                cfg->invalid = true;
                            }
                        } else if (std::strcmp(key, "watermark_pct") == 0) {
                            if (ParseDecimalU32(val, &v) && v >= 1U && v <= 99U) {
                                cfg->watermark_pct = v;
                                has_v2_key = true;
                            } else {
                                cfg->invalid = true;
                            }
                        } else if (std::strcmp(key, "part_max_mb") == 0) {
                            if (ParseDecimalU32(val, &v) && v >= 64U && v <= 4092U) {
                                cfg->part_max_mb = v;
                            } else {
                                cfg->invalid = true;
                            }
                        } else if (std::strcmp(key, "ephemeral") == 0) {
                            if (ParseDecimalU32(val, &v)) {
                                cfg->ephemeral = (v != 0U) ? 1U : 0U;
                            }
                        } else if (std::strcmp(key, "strict_patrol") == 0) {
                            if (ParseDecimalU32(val, &v)) {
                                cfg->strict_patrol = (v != 0U) ? 1U : 0U;
                            }
                        }
                    }
                    p = eol;
                }

                (void)has_v2_key;
                if (!has_size_mb) {
                    cfg->size_mb = (cfg->thin != 0U) ? 512U : 2048U;
                }
                if (cfg->thin != 0U && !has_max_size_mb) {
                    cfg->max_size_auto = true;
                }
                if (cfg->thin != 0U && !has_grow_step_mb) {
                    cfg->grow_step_mb = DefaultGrowStepMb;
                }
            }

            void ReadAndHashPhysicalBase(BaseDigests *out, bool copy_boot_files, const char *emmc_dir, bool compute_hashes) {
                std::memset(out, 0, sizeof(*out));
                constexpr size_t ChunkSize = 512U * 1024U;
                u8 *buf = GetChunkBuffer();

                auto stream_boot_part = [&](fs::IStorage &src_st, const char *dst_path, bool is_boot0) {
                    fs::FileHandle fh{};
                    bool write_file = (copy_boot_files && dst_path != nullptr);
                    if (write_file) {
                        if (const Result rc = fs::CreateFile(dst_path, static_cast<s64>(BootPartBytes)); R_FAILED(rc)) {
                            ShowFatalError("COW: create %s failed: 0x%08" PRIx32 "\n", dst_path, rc.GetValue());
                        }
                        if (const Result rc = fs::OpenFile(std::addressof(fh), dst_path, fs::OpenMode_ReadWrite); R_FAILED(rc)) {
                            ShowFatalError("COW: open %s failed: 0x%08" PRIx32 "\n", dst_path, rc.GetValue());
                        }
                    }
                    ON_SCOPE_EXIT { if (write_file) fs::CloseFile(fh); };

                    Sha256Context full_ctx{};
                    Sha256Context patrol_ctx{};
                    full_ctx.Init();
                    patrol_ctx.Init();

                    for (size_t off = 0; off < BootPartBytes; off += ChunkSize) {
                        const size_t cur = std::min(ChunkSize, BootPartBytes - off);
                        if (const Result rc = src_st.Read(static_cast<s64>(off), buf, cur); R_FAILED(rc)) {
                            ShowFatalError("COW: mmc boot read failed: 0x%08" PRIx32 "\n", rc.GetValue());
                        }
                        if (write_file) {
                            if (const Result rc = fs::WriteFile(fh, static_cast<s64>(off), buf, cur, fs::WriteOption::None); R_FAILED(rc)) {
                                ShowFatalError("COW: write %s failed: 0x%08" PRIx32 "\n", dst_path, rc.GetValue());
                            }
                        }
                        if (compute_hashes) {
                            if (is_boot0) {
                                const size_t p_lo = std::max(off, Boot0PatrolOffset);
                                const size_t p_hi = std::min(off + cur, Boot0PatrolOffset + Boot0PatrolSize);
                                if (p_lo < p_hi) {
                                    patrol_ctx.Update(buf + (p_lo - off), p_hi - p_lo);
                                }
                                const size_t b_lo = std::max(off, Boot0BctSaveOffset);
                                const size_t b_hi = std::min(off + cur, Boot0BctSaveOffset + Boot0BctSaveSize);
                                if (b_lo < b_hi) {
                                    std::memset(buf + (b_lo - off), 0, b_hi - b_lo);
                                }
                                if (p_lo < p_hi) {
                                    std::memset(buf + (p_lo - off), 0, p_hi - p_lo);
                                }
                            }
                            full_ctx.Update(buf, cur);
                        }
                    }
                    if (write_file) {
                        R_DISCARD(fs::FlushFile(fh));
                    }
                    if (compute_hashes) {
                        if (is_boot0) {
                            u8 patrol_full[32];
                            patrol_ctx.Final(patrol_full);
                            std::memcpy(out->boot0_patrol_sha256_16, patrol_full, sizeof(out->boot0_patrol_sha256_16));
                            full_ctx.Final(out->boot0_pkg1_sha256);
                        } else {
                            full_ctx.Final(out->boot1_sha256);
                        }
                    }
                };

                char fpath[0x340];
                if (copy_boot_files || compute_hashes) {
                    if (copy_boot_files && emmc_dir != nullptr) {
                        std::snprintf(fpath, sizeof(fpath), "%s/boot0", emmc_dir);
                    }
                    stream_boot_part(g_mmc_boot0_storage, (copy_boot_files && emmc_dir != nullptr) ? fpath : nullptr, true);

                    if (copy_boot_files && emmc_dir != nullptr) {
                        std::snprintf(fpath, sizeof(fpath), "%s/boot1", emmc_dir);
                    }
                    stream_boot_part(g_mmc_boot1_storage, (copy_boot_files && emmc_dir != nullptr) ? fpath : nullptr, false);
                }

                /* Read physical LBA 0..33 (MBR + Primary GPT) */
                constexpr size_t GptSpanBytes = 34U * SectorSize;
                if (const Result rc = g_mmc_user_storage.Read(0, buf, GptSpanBytes); R_FAILED(rc)) {
                    ShowFatalError("COW: read physical GPT failed: 0x%08" PRIx32 "\n", rc.GetValue());
                }
                if (compute_hashes) {
                    Sha256Context gpt_ctx{};
                    gpt_ctx.Init();
                    gpt_ctx.Update(buf, GptSpanBytes);
                    gpt_ctx.Final(out->gpt_sha256);
                }

                Gpt *gpt = GetGptBuffer();
                std::memcpy(gpt, buf + SectorSize, sizeof(*gpt));
                if (std::memcmp(gpt->header.signature, "EFI PART", 8) == 0 &&
                    gpt->header.number_of_partition_entries <= util::size(gpt->entries)) {
                    out->gpt_valid = true;
                    out->phys_gpp_sectors = gpt->header.alt_lba + 1ULL;

                    u64 pkg2_start = 0ULL;
                    u64 pkg2_sects = 0ULL;
                    for (u32 i = 0; i < gpt->header.number_of_partition_entries; ++i) {
                        if (std::memcmp(gpt->entries[i].partition_name, Package2PartitionName, sizeof(Package2PartitionName)) == 0) {
                            if (gpt->entries[i].ending_lba >= gpt->entries[i].starting_lba &&
                                gpt->entries[i].ending_lba < PrefixSectors) {
                                out->pkg2_valid = true;
                                pkg2_start = gpt->entries[i].starting_lba;
                                pkg2_sects = gpt->entries[i].ending_lba - gpt->entries[i].starting_lba + 1ULL;
                            }
                            break;
                        }
                    }

                    if (compute_hashes && out->pkg2_valid) {
                        Sha256Context pkg2_ctx{};
                        pkg2_ctx.Init();
                        const u64 pkg2_off = pkg2_start * SectorSize;
                        const u64 pkg2_len = pkg2_sects * SectorSize;
                        for (u64 off = 0; off < pkg2_len; off += ChunkSize) {
                            const size_t cur = static_cast<size_t>(std::min<u64>(ChunkSize, pkg2_len - off));
                            if (const Result rc = g_mmc_user_storage.Read(static_cast<s64>(pkg2_off + off), buf, cur); R_FAILED(rc)) {
                                ShowFatalError("COW: read physical BCPKG2 failed: 0x%08" PRIx32 "\n", rc.GetValue());
                            }
                            pkg2_ctx.Update(buf, cur);
                        }
                        pkg2_ctx.Final(out->bcpkg2_sha256);
                    }
                }
            }

            void FormatPartPath(char *out, size_t out_sz, const char *emmc_dir, u32 part_idx) {
                std::snprintf(out, out_sz, "%s/%02" PRIu32, emmc_dir, part_idx);
            }

            bool ScanSplitParts(const char *emmc_dir, u64 *part_sizes, u32 max_parts, u32 *out_num_parts, u64 *out_total_bytes) {
                char fpath[0x340];
                FILINFO fno{};
                u32 count = 0U;
                u64 total = 0ULL;
                for (u32 i = 0; i < max_parts; ++i) {
                    FormatPartPath(fpath, sizeof(fpath), emmc_dir, i);
                    if (::f_stat(fpath, std::addressof(fno)) != FR_OK) {
                        break;
                    }
                    if (fno.fsize == 0) {
                        return false;
                    }
                    part_sizes[i] = static_cast<u64>(fno.fsize);
                    total += static_cast<u64>(fno.fsize);
                    ++count;
                }
                *out_num_parts = count;
                *out_total_bytes = total;
                return count > 0U;
            }

            bool ArchiveRw(const char *emmc_dir, const u64 *part_sizes, u32 num_parts, u64 abs_off, void *buf, size_t len, bool is_write) {
                u8 *ptr = static_cast<u8 *>(buf);
                size_t rem = len;
                u64 cur_off = abs_off;
                char fpath[0x340];

                for (u32 i = 0; i < num_parts && rem > 0; ++i) {
                    const u64 psz = part_sizes[i];
                    if (cur_off >= psz) {
                        cur_off -= psz;
                        continue;
                    }
                    const u64 avail = psz - cur_off;
                    const size_t chunk = static_cast<size_t>(std::min<u64>(rem, avail));
                    FormatPartPath(fpath, sizeof(fpath), emmc_dir, i);

                    FIL fp{};
                    const BYTE mode = is_write ? (FA_OPEN_EXISTING | FA_READ | FA_WRITE) : FA_READ;
                    if (::f_open(std::addressof(fp), fpath, mode) != FR_OK) {
                        return false;
                    }
                    bool ok = true;
                    if (::f_lseek(std::addressof(fp), static_cast<FSIZE_t>(cur_off)) != FR_OK) {
                        ok = false;
                    } else {
                        UINT done = 0;
                        const FRESULT fr = is_write
                            ? ::f_write(std::addressof(fp), ptr, static_cast<UINT>(chunk), std::addressof(done))
                            : ::f_read(std::addressof(fp), ptr, static_cast<UINT>(chunk), std::addressof(done));
                        if (fr != FR_OK || done != chunk) {
                            ok = false;
                        } else if (is_write && ::f_sync(std::addressof(fp)) != FR_OK) {
                            ok = false;
                        }
                    }
                    ::f_close(std::addressof(fp));
                    if (!ok) {
                        return false;
                    }

                    ptr += chunk;
                    rem -= chunk;
                    cur_off = 0ULL;
                }

                return rem == 0;
            }

            bool ExtendOrCreatePart(const char *fpath, u64 new_size, bool create_new) {
                if (new_size == 0ULL) return false;
                FIL fp{};
                const BYTE mode = create_new ? (FA_CREATE_ALWAYS | FA_READ | FA_WRITE) : (FA_OPEN_EXISTING | FA_READ | FA_WRITE);
                if (::f_open(std::addressof(fp), fpath, mode) != FR_OK) {
                    return false;
                }
                bool ok = false;
                if (create_new && ::f_expand(std::addressof(fp), static_cast<FSIZE_t>(new_size), 1) == FR_OK) {
                    ok = (f_size(std::addressof(fp)) == static_cast<FSIZE_t>(new_size));
                } else {
                    const u8 zero = 0U;
                    UINT bw = 0;
                    if (::f_lseek(std::addressof(fp), static_cast<FSIZE_t>(new_size - 1ULL)) == FR_OK &&
                        f_tell(std::addressof(fp)) == static_cast<FSIZE_t>(new_size - 1ULL) &&
                        ::f_write(std::addressof(fp), std::addressof(zero), 1U, std::addressof(bw)) == FR_OK &&
                        bw == 1U &&
                        ::f_sync(std::addressof(fp)) == FR_OK &&
                        f_size(std::addressof(fp)) == static_cast<FSIZE_t>(new_size)) {
                        ok = true;
                    }
                }
                ::f_close(std::addressof(fp));
                return ok;
            }

            bool ValidateHeader(const CowHeader &hdr, u64 total_archive_bytes, u32 sb_slot, u32 *out_l1_entries, u32 *out_first_data) {
                if (hdr.magic != Magic || hdr.prefix_sectors != PrefixSectors) {
                    return false;
                }
                const u64 prefix_bytes = static_cast<u64>(hdr.prefix_sectors) * SectorSize;
                if (total_archive_bytes <= prefix_bytes) {
                    return false;
                }
                const u64 backing_slots = (total_archive_bytes - prefix_bytes) / BlockBytes;

                if (CalculateCrc32(std::addressof(hdr), offsetof(CowHeader, header_crc32)) != hdr.header_crc32) {
                    return false;
                }
                if (hdr.gpp_total_sectors == 0ULL) {
                    return false;
                }

                if (hdr.version != VersionV2) {
                    return false;
                }
                if (hdr.header_ext_size != SectorSize || hdr.block_shift != BlockShift || hdr.l2_shift != L2Shift) {
                    return false;
                }
                if (hdr.alloc_policy != AllocThick && hdr.alloc_policy != AllocThin) {
                    return false;
                }
                if (hdr.l1_entries < MinL1Entries || hdr.l1_entries > MaxL1Entries) {
                    return false;
                }
                if ((hdr.l1_entries % (SectorSize / sizeof(u32))) != 0U) {
                    return false;
                }
                if (hdr.gpp_total_sectors < hdr.physical_base_sectors) {
                    return false;
                }
                const u32 l1_slots = L1SlotsFor(hdr.l1_entries);
                if (hdr.l1_slot_a < SuperblockSlots || hdr.l1_slot_b < SuperblockSlots) {
                    return false;
                }
                const bool non_overlap =
                    (static_cast<u64>(hdr.l1_slot_a) + l1_slots <= static_cast<u64>(hdr.l1_slot_b)) ||
                    (static_cast<u64>(hdr.l1_slot_b) + l1_slots <= static_cast<u64>(hdr.l1_slot_a));
                if (!non_overlap) return false;
                if (static_cast<u64>(hdr.l1_slot_a) + l1_slots > hdr.first_data_slot ||
                    static_cast<u64>(hdr.l1_slot_b) + l1_slots > hdr.first_data_slot) {
                    return false;
                }
                if (hdr.total_slots <= hdr.first_data_slot) return false;
                if (hdr.max_overlay_slots != 0U && hdr.total_slots > hdr.max_overlay_slots) return false;
                const u32 my_l1_slot = (sb_slot == 0U) ? hdr.l1_slot_a : hdr.l1_slot_b;
                if (static_cast<u64>(my_l1_slot) + l1_slots > backing_slots) return false;
                const u32 l1_entries = hdr.l1_entries;
                const u32 first_data = hdr.first_data_slot;

                const u64 max_l1_sectors = static_cast<u64>(l1_entries) * L2Entries * BlockSectors;
                if (max_l1_sectors < hdr.gpp_total_sectors) return false;
                if (static_cast<u64>(hdr.total_slots) > backing_slots) return false;
                if (hdr.next_free_slot < first_data || hdr.next_free_slot > hdr.total_slots) return false;

                *out_l1_entries = l1_entries;
                *out_first_data = first_data;
                return true;
            }

            bool CommitSuperblock(const char *emmc_dir, const u64 *part_sizes, u32 num_parts, u32 target_sb_slot, CowHeader *hdr, const u32 *l1_table) {
                u8 *slot_buf = GetSlotBuffer();
                const u64 prefix_bytes = static_cast<u64>(hdr->prefix_sectors) * SectorSize;
                const u64 sb_off = prefix_bytes + (static_cast<u64>(target_sb_slot) * BlockBytes);

                const u32 l1_bytes = hdr->l1_entries * static_cast<u32>(sizeof(u32));
                const u32 l1_slots = L1SlotsFor(hdr->l1_entries);
                const u32 target_l1_slot = (target_sb_slot == 0U) ? hdr->l1_slot_a : hdr->l1_slot_b;
                const u64 l1_off = prefix_bytes + (static_cast<u64>(target_l1_slot) * BlockBytes);

                for (u32 s = 0; s < l1_slots; ++s) {
                    std::memset(slot_buf, 0, BlockBytes);
                    const u32 byte_off = s * BlockBytes;
                    if (byte_off < l1_bytes) {
                        const u32 take = std::min<u32>(l1_bytes - byte_off, BlockBytes);
                        std::memcpy(slot_buf, reinterpret_cast<const u8 *>(l1_table) + byte_off, take);
                    }
                    if (!ArchiveRw(emmc_dir, part_sizes, num_parts, l1_off + (static_cast<u64>(s) * BlockBytes), slot_buf, BlockBytes, true)) {
                        return false;
                    }
                }

                FillSuperblockV2(slot_buf, *hdr, l1_table, hdr->seq_num);
                std::memcpy(hdr, slot_buf, sizeof(*hdr));
                return ArchiveRw(emmc_dir, part_sizes, num_parts, sb_off, slot_buf, BlockBytes, true);
            }

            void AutoPreBootCheckAndGrow(const char *base_path, size_t base_len) {
                char emmc_dir[0x320];
                std::snprintf(emmc_dir, sizeof(emmc_dir), "%.*s/eMMC", static_cast<int>(base_len), base_path);

                u64 part_sizes[MaxSplitParts]{};
                u32 num_parts = 0;
                u64 total_archive_bytes = 0;
                if (!ScanSplitParts(emmc_dir, part_sizes, MaxSplitParts, std::addressof(num_parts), std::addressof(total_archive_bytes))) {
                    return;
                }

                if (total_archive_bytes < PrefixBytes + static_cast<u64>(SuperblockSlots) * BlockBytes) {
                    return;
                }

                CowHeader probe[SuperblockSlots]{};
                bool has_magic = false;
                for (u32 s = 0; s < SuperblockSlots; ++s) {
                    if (!ArchiveRw(emmc_dir, part_sizes, num_parts,
                                   PrefixBytes + (static_cast<u64>(s) * BlockBytes),
                                   std::addressof(probe[s]), sizeof(probe[s]), false)) {
                        ShowFatalError("COW PreBoot: failed to read superblock slot %" PRIu32 "\n", s);
                    }
                    if (probe[s].magic == Magic) {
                        has_magic = true;
                    }
                }

                /* Non-COW plain file-based emuMMC -> do nothing */
                if (!has_magic) {
                    return;
                }

                u32 hdr_l1_ents[SuperblockSlots]{};
                u32 hdr_first_data[SuperblockSlots]{};
                bool hdr_ok[SuperblockSlots]{};
                for (u32 s = 0; s < SuperblockSlots; ++s) {
                    hdr_ok[s] = ValidateHeader(probe[s], total_archive_bytes, s, std::addressof(hdr_l1_ents[s]), std::addressof(hdr_first_data[s]));
                }
                if (!hdr_ok[0] && !hdr_ok[1]) {
                    ShowFatalError("COW PreBoot: both COW0 superblocks are invalid!\n");
                }

                u32 order[SuperblockSlots] = { 0U, 1U };
                if (!(hdr_ok[0] && (!hdr_ok[1] || probe[0].seq_num >= probe[1].seq_num))) {
                    order[0] = 1U;
                    order[1] = 0U;
                }

                u32 *l1_table = GetL1Buffer();
                bool mounted = false;
                u32 active_slot = 0U;
                CowHeader hdr{};

                for (u32 i = 0; i < SuperblockSlots; ++i) {
                    const u32 s = order[i];
                    if (!hdr_ok[s]) continue;

                    const u32 l1_bytes = hdr_l1_ents[s] * static_cast<u32>(sizeof(u32));
                    const u64 prefix_bytes = static_cast<u64>(probe[s].prefix_sectors) * SectorSize;
                    const u64 l1_off = prefix_bytes + (static_cast<u64>((s == 0U) ? probe[s].l1_slot_a : probe[s].l1_slot_b) * BlockBytes);

                    std::memset(l1_table, 0, MaxL1Entries * sizeof(u32));
                    if (!ArchiveRw(emmc_dir, part_sizes, num_parts, l1_off, l1_table, l1_bytes, false)) {
                        continue;
                    }
                    if (CalculateCrc32(l1_table, l1_bytes) == probe[s].l1_crc32) {
                        active_slot = s;
                        hdr = probe[s];
                        mounted = true;
                        break;
                    }
                }

                if (!mounted) {
                    ShowFatalError("COW PreBoot: L1 CRC32 verification failed!\n");
                }
                if ((hdr.flags & FlagFwMismatch) != 0U) {
                    ShowFatalError("COW PreBoot: COW_FLAG_FW_MISMATCH set! Spawn/rebase overlay before booting.\n");
                }
                if ((hdr.flags & FlagConflictLocked) != 0U) {
                    ShowFatalError("COW PreBoot: COW_FLAG_CONFLICT_LOCKED set! Reconcile overlay before booting.\n");
                }

                AutoInitConfig cfg{};
                char auto_cfg[0x320];
                std::snprintf(auto_cfg, sizeof(auto_cfg), "%.*s/cow_autoinit", static_cast<int>(base_len), base_path);
                ParseAutoInitConfig(auto_cfg, std::addressof(cfg));

                bool need_sb_commit = false;

                const bool chk_pkg1   = !IsZero(hdr.boot0_pkg1_sha256, sizeof(hdr.boot0_pkg1_sha256));
                const bool chk_patrol = !IsZero(hdr.boot0_patrol_sha256_16, sizeof(hdr.boot0_patrol_sha256_16));
                const bool chk_boot1  = !IsZero(hdr.boot1_sha256, sizeof(hdr.boot1_sha256));
                const bool chk_gpt    = !IsZero(hdr.gpt_sha256, sizeof(hdr.gpt_sha256));
                const bool chk_pkg2   = !IsZero(hdr.bcpkg2_sha256, sizeof(hdr.bcpkg2_sha256));

                if (chk_pkg1 || chk_patrol || chk_boot1 || chk_gpt || chk_pkg2) {
                    EnsureMmcInitialized();
                    BaseDigests cur{};
                    ReadAndHashPhysicalBase(std::addressof(cur), false, nullptr, true);

                    u32 hard_drift = 0U;
                    if (chk_pkg1 && std::memcmp(cur.boot0_pkg1_sha256, hdr.boot0_pkg1_sha256, 32) != 0) {
                        hard_drift |= DriftBoot0Pkg1;
                    }
                    if (chk_boot1 && std::memcmp(cur.boot1_sha256, hdr.boot1_sha256, 32) != 0) {
                        hard_drift |= DriftBoot1;
                    }
                    if (chk_gpt && (!cur.gpt_valid || std::memcmp(cur.gpt_sha256, hdr.gpt_sha256, 32) != 0)) {
                        hard_drift |= DriftGpt;
                    }
                    if (chk_pkg2 && (!cur.pkg2_valid || std::memcmp(cur.bcpkg2_sha256, hdr.bcpkg2_sha256, 32) != 0)) {
                        hard_drift |= DriftBcpkg2;
                    }

                    const bool patrol_changed = chk_patrol &&
                        (std::memcmp(cur.boot0_patrol_sha256_16, hdr.boot0_patrol_sha256_16, 16) != 0);

                    if (hard_drift != 0U) {
                        hdr.flags |= FlagFwMismatch;
                        hdr.base_drift_part_mask |= hard_drift | (patrol_changed ? DriftBoot0Patrol : 0U);
                        hdr.seq_num++;
                        const u32 next_slot = active_slot ^ 1U;
                        (void)CommitSuperblock(emmc_dir, part_sizes, num_parts, next_slot, std::addressof(hdr), l1_table);
                        ShowFatalError("COW PreBoot: physical sysNAND FW/layout drifted (mask=0x%08" PRIx32 ")! Mount blocked.\n",
                                       hdr.base_drift_part_mask);
                    }

                    if (patrol_changed) {
                        const u32 new_mask = hdr.base_drift_part_mask | DriftBoot0Patrol;
                        const u32 new_flags = hdr.flags | (cfg.strict_patrol ? FlagNeedsRebase : 0U);
                        if (new_mask != hdr.base_drift_part_mask || new_flags != hdr.flags) {
                            hdr.base_drift_part_mask = new_mask;
                            hdr.flags = new_flags;
                            need_sb_commit = true;
                        }
                    }
                }

                if (hdr.alloc_policy == AllocThin && hdr.max_overlay_slots > hdr.total_slots) {
                    const u32 data_start = hdr.first_data_slot;
                    const u32 used_slots = (hdr.next_free_slot >= data_start) ? (hdr.next_free_slot - data_start) : 0U;
                    const u32 backed_slots = (hdr.total_slots >= data_start) ? (hdr.total_slots - data_start) : 0U;
                    u32 pct = hdr.thin_high_watermark_pct ? static_cast<u32>(hdr.thin_high_watermark_pct) : DefaultWatermarkPct;
                    if (pct > 100U) pct = 100U;

                    if (backed_slots > 0U && (static_cast<u64>(used_slots) * 100ULL >= static_cast<u64>(backed_slots) * pct)) {
                        u64 free_bytes = 0ULL;
                        if (GetSdFreeBytes(std::addressof(free_bytes))) {
                            const u64 reserve_bytes = static_cast<u64>(cfg.sd_reserve_mb) * 1024ULL * 1024ULL;
                            const u64 safe_free_bytes = (free_bytes > reserve_bytes) ? (free_bytes - reserve_bytes) : 0ULL;
                            const u64 safe_free_slots64 = safe_free_bytes / BlockBytes;
                            const u32 safe_free_slots = (safe_free_slots64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : static_cast<u32>(safe_free_slots64);

                            const u32 step_slots = hdr.thin_grow_step_slots ? hdr.thin_grow_step_slots : (DefaultGrowStepMb * 64U);
                            const u32 cap_slots = hdr.max_overlay_slots - hdr.total_slots;
                            const u32 grow_slots = std::min({ step_slots, cap_slots, safe_free_slots });

                            if (grow_slots > 0U) {
                                u64 cfg_part_max = hdr.part_max_sectors
                                    ? (static_cast<u64>(hdr.part_max_sectors) * SectorSize)
                                    : (static_cast<u64>(DefaultPartMaxMb) * 1024ULL * 1024ULL);
                                if (cfg_part_max > static_cast<u64>(DefaultPartMaxMb) * 1024ULL * 1024ULL) {
                                    cfg_part_max = static_cast<u64>(DefaultPartMaxMb) * 1024ULL * 1024ULL;
                                }
                                if (cfg_part_max < part_sizes[0]) {
                                    cfg_part_max = part_sizes[0];
                                }

                                const u64 uniform_part_bytes = (num_parts == 1U) ? cfg_part_max : part_sizes[0];
                                const u64 max_arch_bytes = static_cast<u64>(MaxSplitParts) * uniform_part_bytes;
                                const u64 prefix_bytes = static_cast<u64>(hdr.prefix_sectors) * SectorSize;
                                u64 target_total_bytes = prefix_bytes + (static_cast<u64>(hdr.total_slots + grow_slots) * BlockBytes);
                                if (target_total_bytes > max_arch_bytes) {
                                    target_total_bytes = max_arch_bytes;
                                }

                                if (target_total_bytes > total_archive_bytes) {
                                    u64 bytes_to_add = target_total_bytes - total_archive_bytes;
                                    const u32 last_idx = num_parts - 1U;
                                    char ppath[0x340];

                                    if (part_sizes[last_idx] < uniform_part_bytes) {
                                        const u64 room = uniform_part_bytes - part_sizes[last_idx];
                                        const u64 ext = std::min(bytes_to_add, room);
                                        const u64 new_sz = part_sizes[last_idx] + ext;
                                        FormatPartPath(ppath, sizeof(ppath), emmc_dir, last_idx);
                                        if (ExtendOrCreatePart(ppath, new_sz, false)) {
                                            part_sizes[last_idx] = new_sz;
                                            total_archive_bytes += ext;
                                            bytes_to_add -= ext;
                                        } else {
                                            bytes_to_add = 0ULL;
                                        }
                                    }

                                    while (bytes_to_add > 0ULL && num_parts < MaxSplitParts) {
                                        const u64 new_sz = std::min(bytes_to_add, uniform_part_bytes);
                                        FormatPartPath(ppath, sizeof(ppath), emmc_dir, num_parts);
                                        if (!ExtendOrCreatePart(ppath, new_sz, true)) {
                                            ::f_unlink(ppath);
                                            break;
                                        }
                                        part_sizes[num_parts] = new_sz;
                                        ++num_parts;
                                        total_archive_bytes += new_sz;
                                        bytes_to_add -= new_sz;
                                    }
                                }

                                const u64 new_backing_slots64 = (total_archive_bytes - prefix_bytes) / BlockBytes;
                                u32 new_total_slots = static_cast<u32>(std::min<u64>(static_cast<u64>(hdr.total_slots) + grow_slots, new_backing_slots64));
                                if (new_total_slots > hdr.max_overlay_slots) {
                                    new_total_slots = hdr.max_overlay_slots;
                                }
                                if (new_total_slots > hdr.total_slots) {
                                    hdr.total_slots = new_total_slots;
                                    need_sb_commit = true;
                                }
                            }
                        }
                    }
                }

                if (need_sb_commit) {
                    hdr.seq_num++;
                    const u32 next_slot = active_slot ^ 1U;
                    if (!CommitSuperblock(emmc_dir, part_sizes, num_parts, next_slot, std::addressof(hdr), l1_table)) {
                        ShowFatalError("COW PreBoot: failed to commit updated superblock!\n");
                    }
                }
            }

            bool AutoInitCowOverlay(const char *base_path, size_t base_len) {
                char auto_cfg[0x320];
                char emmc_dir[0x320];
                char fpath[0x340];

                std::snprintf(auto_cfg, sizeof(auto_cfg), "%.*s/cow_autoinit", static_cast<int>(base_len), base_path);
                if (!PathExists(auto_cfg)) {
                    return false;
                }
                std::snprintf(emmc_dir, sizeof(emmc_dir), "%.*s/eMMC", static_cast<int>(base_len), base_path);
                std::snprintf(fpath, sizeof(fpath), "%s/00", emmc_dir);
                if (PathExists(fpath)) {
                    return false;
                }

                AutoInitConfig cfg{};
                ParseAutoInitConfig(auto_cfg, std::addressof(cfg));
                if (cfg.invalid) {
                    ShowFatalError("COW AutoInit: invalid cow_autoinit configuration\n");
                }

                EnsureMmcInitialized();

                if (const Result rc = EnsureDirectoryHierarchy(emmc_dir); R_FAILED(rc)) {
                    ShowFatalError("COW AutoInit: mkdir %s failed: 0x%08" PRIx32 "\n", emmc_dir, rc.GetValue());
                }
                std::snprintf(fpath, sizeof(fpath), "%.*s/Nintendo", static_cast<int>(base_len), base_path);
                R_DISCARD(EnsureDirectoryHierarchy(fpath));

                u64 sd_free_bytes = 0ULL;
                if (!GetSdFreeBytes(std::addressof(sd_free_bytes))) {
                    ShowFatalError("COW AutoInit: f_getfree failed\n");
                }
                const u64 reserve_bytes = static_cast<u64>(cfg.sd_reserve_mb) * 1024ULL * 1024ULL;
                const u64 overhead_bytes = (static_cast<u64>(BootPartBytes) * 2ULL) + PrefixBytes;
                if (sd_free_bytes <= reserve_bytes + overhead_bytes) {
                    ShowFatalError("COW AutoInit: insufficient SD free space (respecting sd_reserve_mb)\n");
                }
                const u64 max_safe_pool_bytes = ((sd_free_bytes - reserve_bytes - overhead_bytes) / BlockBytes) * BlockBytes;
                const u64 max_safe_pool_slots64 = max_safe_pool_bytes / BlockBytes;
                const u32 max_safe_pool_slots = (max_safe_pool_slots64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : static_cast<u32>(max_safe_pool_slots64);

                u32 total_slots = 0U;
                if (cfg.size_auto) {
                    const u32 target_mb = (cfg.thin != 0U) ? 512U : 2048U;
                    const u64 target_slots64 = (static_cast<u64>(target_mb) * 1024ULL * 1024ULL) / BlockBytes;
                    total_slots = static_cast<u32>(std::min<u64>(target_slots64, max_safe_pool_slots));
                } else {
                    const u64 req_bytes = ((static_cast<u64>(cfg.size_mb) * 1024ULL * 1024ULL) / BlockBytes) * BlockBytes;
                    const u64 req_slots64 = req_bytes / BlockBytes;
                    if (req_slots64 > max_safe_pool_slots) {
                        ShowFatalError("COW AutoInit: requested size_mb exceeds safe SD free space\n");
                    }
                    total_slots = static_cast<u32>(req_slots64);
                }
                if (total_slots <= FirstDataSlot) {
                    ShowFatalError("COW AutoInit: COW pool too small (%" PRIu32 " slots)\n", total_slots);
                }

                BaseDigests digests{};
                ReadAndHashPhysicalBase(std::addressof(digests), true, emmc_dir, true);
                if (!digests.gpt_valid) {
                    ShowFatalError("COW AutoInit: invalid physical GPT\n");
                }
                if (!digests.pkg2_valid) {
                    ShowFatalError("COW AutoInit: BCPKG2-1-Normal-Main missing or exceeds 32 MiB prefix!\n");
                }

                const u64 phys_gpp_sectors = digests.phys_gpp_sectors;
                u64 gpp_total_sectors = phys_gpp_sectors;
                if (cfg.virtual_gb > 0U) {
                    const u64 virt_sects = static_cast<u64>(cfg.virtual_gb) * 2097152ULL;
                    if (virt_sects < phys_gpp_sectors) {
                        ShowFatalError("COW AutoInit: virtual_gb smaller than physical sysNAND\n");
                    }
                    gpp_total_sectors = virt_sects;
                }

                constexpr u64 SectorsPerL1 = static_cast<u64>(L2Entries) * BlockSectors;
                const u64 needed_l1 = (gpp_total_sectors + SectorsPerL1 - 1ULL) / SectorsPerL1;
                if (needed_l1 > MaxL1Entries) {
                    ShowFatalError("COW AutoInit: virtual GPP exceeds 512 GiB max v2 L1 capacity\n");
                }
                const u32 l1_entries = (needed_l1 <= V2DefaultL1Entries) ? V2DefaultL1Entries : MaxL1Entries;
                const u32 l1_slots = L1SlotsFor(l1_entries);
                const u32 l1_slot_a = SuperblockSlots;
                const u32 l1_slot_b = l1_slot_a + l1_slots;
                const u32 first_data_slot = l1_slot_b + l1_slots;
                u32 max_overlay_slots = total_slots;

                if (total_slots <= first_data_slot) {
                    ShowFatalError("COW AutoInit: pool too small for v2 L1 slots\n");
                }

                if (cfg.thin != 0U) {
                    if (cfg.max_size_auto) {
                        max_overlay_slots = max_safe_pool_slots;
                    } else if (cfg.max_size_mb > 0U) {
                        const u64 max64 = (static_cast<u64>(cfg.max_size_mb) * 1024ULL * 1024ULL) / BlockBytes;
                        max_overlay_slots = (max64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : static_cast<u32>(max64);
                    }
                } else if (cfg.max_size_mb > 0U && !cfg.max_size_auto) {
                    const u64 max64 = (static_cast<u64>(cfg.max_size_mb) * 1024ULL * 1024ULL) / BlockBytes;
                    max_overlay_slots = (max64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : static_cast<u32>(max64);
                }
                if (max_overlay_slots < total_slots) {
                    ShowFatalError("COW AutoInit: max_size_mb smaller than size_mb\n");
                }

                const u64 cow_pool_bytes = static_cast<u64>(total_slots) * BlockBytes;
                u64 remaining            = static_cast<u64>(PrefixBytes) + cow_pool_bytes;
                const u64 part_max_bytes = static_cast<u64>(cfg.part_max_mb) * 1024ULL * 1024ULL;
                u32 flags                = (cfg.ephemeral != 0U) ? FlagEphemeral : 0U;
                if (cfg.thin != 0U) {
                    flags |= FlagThinProvision;
                }

                CowHeader v2_tmpl{};
                v2_tmpl.magic                   = Magic;
                v2_tmpl.version                 = VersionV2;
                v2_tmpl.gpp_total_sectors       = gpp_total_sectors;
                v2_tmpl.prefix_sectors          = PrefixSectors;
                v2_tmpl.total_slots             = total_slots;
                v2_tmpl.next_free_slot          = first_data_slot;
                v2_tmpl.flags                   = flags;
                v2_tmpl.header_ext_size         = SectorSize;
                v2_tmpl.physical_base_sectors   = phys_gpp_sectors;
                v2_tmpl.max_overlay_slots       = max_overlay_slots;
                v2_tmpl.l1_entries              = l1_entries;
                v2_tmpl.l1_slot_a               = l1_slot_a;
                v2_tmpl.l1_slot_b               = l1_slot_b;
                v2_tmpl.first_data_slot         = first_data_slot;
                v2_tmpl.thin_grow_step_slots    = (cfg.thin != 0U) ? static_cast<u32>((static_cast<u64>(cfg.grow_step_mb) * 1024ULL * 1024ULL) / BlockBytes) : 0U;
                v2_tmpl.thin_high_watermark_pct = static_cast<u16>(cfg.watermark_pct);
                v2_tmpl.alloc_policy            = (cfg.thin != 0U) ? AllocThin : AllocThick;
                v2_tmpl.block_shift             = BlockShift;
                v2_tmpl.l2_shift                = L2Shift;
                v2_tmpl.part_max_sectors        = cfg.part_max_mb * 2048U;
                std::memcpy(v2_tmpl.boot0_pkg1_sha256, digests.boot0_pkg1_sha256, 32);
                std::memcpy(v2_tmpl.boot0_patrol_sha256_16, digests.boot0_patrol_sha256_16, 16);
                std::memcpy(v2_tmpl.boot1_sha256, digests.boot1_sha256, 32);
                std::memcpy(v2_tmpl.gpt_sha256, digests.gpt_sha256, 32);
                std::memcpy(v2_tmpl.bcpkg2_sha256, digests.bcpkg2_sha256, 32);
                ExtractOverlayName(base_path, base_len, v2_tmpl.overlay_name, sizeof(v2_tmpl.overlay_name));

                u64 gen_acc = phys_gpp_sectors ^ 0x434F573247454E31ULL;
                for (u32 k = 0; k < 4U; ++k) {
                    u64 w = 0ULL;
                    std::memcpy(&w, digests.boot0_pkg1_sha256 + k * 8U, 8);
                    gen_acc ^= w;
                    std::memcpy(&w, digests.bcpkg2_sha256 + k * 8U, 8);
                    gen_acc = (gen_acc << 7) | (gen_acc >> 57);
                    gen_acc ^= w;
                }
                v2_tmpl.base_gen_id = gen_acc ? gen_acc : 1ULL;
                v2_tmpl.last_verified_base_gen_id = v2_tmpl.base_gen_id;

                constexpr size_t ChunkSize = 512U * 1024U;
                u8 *buf    = GetChunkBuffer();
                u8 *sb_buf = GetSlotBuffer();

                for (u32 part_idx = 0; remaining > 0ULL; ++part_idx) {
                    if (part_idx >= MaxSplitParts) {
                        ShowFatalError("COW AutoInit: exceeded max split parts (%" PRIu32 ")\n", MaxSplitParts);
                    }
                    const u64 cur_size = std::min(remaining, part_max_bytes);
                    FormatPartPath(fpath, sizeof(fpath), emmc_dir, part_idx);
                    if (!ExtendOrCreatePart(fpath, cur_size, true)) {
                        ShowFatalError("COW AutoInit: create %s failed\n", fpath);
                    }
                    if (part_idx == 0U) {
                        fs::FileHandle fh{};
                        if (const Result rc = fs::OpenFile(std::addressof(fh), fpath, fs::OpenMode_ReadWrite); R_FAILED(rc)) {
                            ShowFatalError("COW AutoInit: open %s failed: 0x%08" PRIx32 "\n", fpath, rc.GetValue());
                        }
                        ON_SCOPE_EXIT { fs::CloseFile(fh); };
                        for (size_t off = 0; off < PrefixBytes; off += ChunkSize) {
                            const size_t cur = std::min(ChunkSize, PrefixBytes - off);
                            if (const Result rc = g_mmc_user_storage.Read(static_cast<s64>(off), buf, cur); R_FAILED(rc)) {
                                ShowFatalError("COW AutoInit: read GPP prefix failed: 0x%08" PRIx32 "\n", rc.GetValue());
                            }
                            if (const Result rc = fs::WriteFile(fh, static_cast<s64>(off), buf, cur, fs::WriteOption::None); R_FAILED(rc)) {
                                ShowFatalError("COW AutoInit: write prefix failed: 0x%08" PRIx32 "\n", rc.GetValue());
                            }
                        }
                        FillSuperblockV2(sb_buf, v2_tmpl, nullptr, 1ULL);
                        if (const Result rc = fs::WriteFile(fh, static_cast<s64>(PrefixBytes), sb_buf, BlockBytes, fs::WriteOption::None); R_FAILED(rc)) {
                            ShowFatalError("COW AutoInit: write v2 SB0 failed: 0x%08" PRIx32 "\n", rc.GetValue());
                        }
                        FillSuperblockV2(sb_buf, v2_tmpl, nullptr, 2ULL);
                        if (const Result rc = fs::WriteFile(fh, static_cast<s64>(PrefixBytes + BlockBytes), sb_buf, BlockBytes, fs::WriteOption::None); R_FAILED(rc)) {
                            ShowFatalError("COW AutoInit: write v2 SB1 failed: 0x%08" PRIx32 "\n", rc.GetValue());
                        }
                        std::memset(sb_buf, 0, BlockBytes);
                        for (u32 s = 0; s < 2U * l1_slots; ++s) {
                            const s64 l1_off = static_cast<s64>(PrefixBytes + (static_cast<u64>(SuperblockSlots + s) * BlockBytes));
                            if (const Result rc = fs::WriteFile(fh, l1_off, sb_buf, BlockBytes, fs::WriteOption::None); R_FAILED(rc)) {
                                ShowFatalError("COW AutoInit: write v2 zero L1 failed: 0x%08" PRIx32 "\n", rc.GetValue());
                            }
                        }
                        R_DISCARD(fs::FlushFile(fh));
                    }
                    remaining -= cur_size;
                }

                return true;
            }

        }

    }

    void InitializeEmummc(bool emummc_enabled, const secmon::EmummcConfiguration &emummc_cfg) {
        Result result;
        if (emummc_enabled) {
            /* Get sd card size. */
            s64 sd_card_size;
            if (R_FAILED((result = g_sd_card_storage.GetSize(std::addressof(sd_card_size))))) {
                ShowFatalError("Failed to get sd card size: 0x%08" PRIx32 "!\n", result.GetValue());
            }

            if (emummc_cfg.base_cfg.type == secmon::EmummcType_Partition) {
                const s64 partition_start = emummc_cfg.partition_cfg.start_sector * sdmmc::SectorSize;
                g_boot0_storage = AllocateObject<fs::SubStorage>(g_sd_card_storage, partition_start, 4_MB);
                g_user_storage  = AllocateObject<fs::SubStorage>(g_sd_card_storage, partition_start + 8_MB, sd_card_size - (partition_start + 8_MB));
            } else if (emummc_cfg.base_cfg.type == secmon::EmummcType_File) {
                /* Get the base emummc path. */
                std::memcpy(g_emummc_path, emummc_cfg.file_cfg.path.str, sizeof(emummc_cfg.file_cfg.path.str));

                /* Get path length. */
                auto len = std::strlen(g_emummc_path);

                /* Zero-PC auto-initialization if cow_autoinit marker is present and eMMC/00 is missing. */
                if (!cow::AutoInitCowOverlay(g_emummc_path, len)) {
                    /* Phase 2 pre-boot integrity, base SHA-256 verification, and SD-safe thin pool growth. */
                    cow::AutoPreBootCheckAndGrow(g_emummc_path, len);
                }

                /* Append emmc. */
                std::memcpy(g_emummc_path + len, "/eMMC", 6);
                len += 5;

                /* Open boot0. */
                fs::FileHandle boot0_file;
                std::memcpy(g_emummc_path + len, "/boot0", 7);
                if (R_FAILED((result = fs::OpenFile(std::addressof(boot0_file), g_emummc_path, fs::OpenMode_Read)))) {
                    ShowFatalError("Failed to open emummc boot0 file: 0x%08" PRIx32 "!\n", result.GetValue());
                }

                /* Open boot1. */
                g_emummc_path[len + 5] = '1';
                {
                    fs::DirectoryEntryType entry_type;
                    bool is_archive;
                    if (R_FAILED((result = fs::GetEntryType(std::addressof(entry_type), std::addressof(is_archive), g_emummc_path)))) {
                        ShowFatalError("Failed to find emummc boot1 file: 0x%08" PRIx32 "!\n", result.GetValue());
                    }

                    if (entry_type != fs::DirectoryEntryType_File) {
                        ShowFatalError("emummc boot1 file is not a file!\n");
                    }
                }

                /* Open userdata. */
                std::memcpy(g_emummc_path + len, "/00", 4);
                fs::FileHandle user00_file;
                if (R_FAILED((result = fs::OpenFile(std::addressof(user00_file), g_emummc_path, fs::OpenMode_Read)))) {
                    ShowFatalError("Failed to open emummc user %02d file: 0x%08" PRIx32 "!\n", 0, result.GetValue());
                }

                /* Create partitions. */
                g_boot0_storage = AllocateObject<fs::FileHandleStorage>(boot0_file);
                g_user_storage  = AllocateObject<EmummcFileStorage>(user00_file, len + 1);
            } else {
                ShowFatalError("Unknown emummc type %d\n", static_cast<int>(emummc_cfg.base_cfg.type));
            }
        } else {
            /* Initialize access to mmc. */
            {
                const Result result = InitializeMmc();
                if (R_FAILED(result)) {
                    ShowFatalError("Failed to initialize mmc: 0x%08" PRIx32 "\n", result.GetValue());
                }
            }

            /* Create storages. */
            g_boot0_storage = std::addressof(g_mmc_boot0_storage);
            g_user_storage  = std::addressof(g_mmc_user_storage);
        }
        if (g_boot0_storage == nullptr) {
            ShowFatalError("Failed to initialize BOOT0\n");
        }
        if (g_user_storage == nullptr) {
            ShowFatalError("Failed to initialize Raw EMMC\n");
        }

        /* Read the GPT. */
        Gpt *gpt = static_cast<Gpt *>(AllocateAligned(sizeof(Gpt), 0x200));
        {
            const Result result = g_user_storage->Read(0x200, gpt, sizeof(*gpt));
            if (R_FAILED(result)) {
                ShowFatalError("Failed to read GPT: 0x%08" PRIx32 "\n", result.GetValue());
            }
        }

        /* Check the GPT. */
        if (std::memcmp(gpt->header.signature, "EFI PART", 8) != 0) {
            ShowFatalError("Invalid GPT signature\n");
        }
        if (gpt->header.number_of_partition_entries > util::size(gpt->entries)) {
            ShowFatalError("Too many GPT entries\n");
        }

        /* Create system storage. */
        for (u32 i = 0; i < gpt->header.number_of_partition_entries; ++i) {
            if (gpt->entries[i].starting_lba < gpt->header.first_usable_lba) {
                continue;
            }

            const s64 offset =  INT64_C(0x200) * gpt->entries[i].starting_lba;
            const u64 size   = UINT64_C(0x200) * (gpt->entries[i].ending_lba + 1 - gpt->entries[i].starting_lba);

            if (std::memcmp(gpt->entries[i].partition_name, Package2PartitionName, sizeof(Package2PartitionName)) == 0) {
                g_package2_storage = AllocateObject<fs::SubStorage>(*g_user_storage, offset, size);
            }
        }

        /* Check that we created package2 storage. */
        if (g_package2_storage == nullptr) {
            ShowFatalError("Failed to initialize Package2\n");
        }
    }

    Result ReadBoot0(s64 offset, void *dst, size_t size) {
        R_RETURN(g_boot0_storage->Read(offset, dst, size));
    }

    Result ReadPackage2(s64 offset, void *dst, size_t size) {
        R_RETURN(g_package2_storage->Read(offset, dst, size));
    }

}
