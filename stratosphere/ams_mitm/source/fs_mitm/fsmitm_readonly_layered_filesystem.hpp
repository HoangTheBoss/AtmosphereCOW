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
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::fs {

    class ReadOnlyLayeredFileSystem : public ams::fs::fsa::IFileSystem {
        private:
            ams::fs::ReadOnlyFileSystem m_fs_1;
            ams::fs::ReadOnlyFileSystem m_fs_2;
        public:
            explicit ReadOnlyLayeredFileSystem(std::unique_ptr<ams::fs::fsa::IFileSystem> a, std::unique_ptr<ams::fs::fsa::IFileSystem> b) : m_fs_1(std::move(a)), m_fs_2(std::move(b)) { /* ... */ }

            virtual ~ReadOnlyLayeredFileSystem() { /* ... */ }
        private:
            virtual Result DoCreateFile(const ams::fs::Path &path, s64 size, int flags) override final {
                AMS_UNUSED(path, size, flags);
                R_THROW(ams::fs::ResultUnsupportedOperation())
            }

            virtual Result DoDeleteFile(const ams::fs::Path &path) override final {
                AMS_UNUSED(path);
                R_THROW(ams::fs::ResultUnsupportedOperation())
            }

            virtual Result DoCreateDirectory(const ams::fs::Path &path) override final {
                AMS_UNUSED(path);
                R_THROW(ams::fs::ResultUnsupportedOperation())
            }

            virtual Result DoDeleteDirectory(const ams::fs::Path &path) override final {
                AMS_UNUSED(path);
                R_THROW(ams::fs::ResultUnsupportedOperation())
            }

            virtual Result DoDeleteDirectoryRecursively(const ams::fs::Path &path) override final {
                AMS_UNUSED(path);
                R_THROW(ams::fs::ResultUnsupportedOperation())
            }

            virtual Result DoRenameFile(const ams::fs::Path &old_path, const ams::fs::Path &new_path) override final {
                AMS_UNUSED(old_path, new_path);
                R_THROW(ams::fs::ResultUnsupportedOperation())
            }

            virtual Result DoRenameDirectory(const ams::fs::Path &old_path, const ams::fs::Path &new_path) override final {
                AMS_UNUSED(old_path, new_path);
                R_THROW(ams::fs::ResultUnsupportedOperation())
            }

            virtual Result DoGetEntryType(ams::fs::DirectoryEntryType *out, const ams::fs::Path &path) override final {
                R_SUCCEED_IF(R_SUCCEEDED(m_fs_1.GetEntryType(out, path)));
                R_RETURN(m_fs_2.GetEntryType(out, path));
            }

            virtual Result DoOpenFile(std::unique_ptr<ams::fs::fsa::IFile> *out_file, const ams::fs::Path &path, ams::fs::OpenMode mode) override final {
                R_SUCCEED_IF(R_SUCCEEDED(m_fs_1.OpenFile(out_file, path, mode)));
                R_RETURN(m_fs_2.OpenFile(out_file, path, mode));
            }

            virtual Result DoOpenDirectory(std::unique_ptr<ams::fs::fsa::IDirectory> *out_dir, const ams::fs::Path &path, ams::fs::OpenDirectoryMode mode) override final {
                R_SUCCEED_IF(R_SUCCEEDED(m_fs_1.OpenDirectory(out_dir, path, mode)));
                R_RETURN(m_fs_2.OpenDirectory(out_dir, path, mode));
            }

            virtual Result DoCommit() override final {
                R_SUCCEED();
            }

            virtual Result DoGetFreeSpaceSize(s64 *out, const ams::fs::Path &path) {
                AMS_UNUSED(out, path);
                R_THROW(ams::fs::ResultUnsupportedOperation())
            }

            virtual Result DoGetTotalSpaceSize(s64 *out, const ams::fs::Path &path) {
                AMS_UNUSED(out, path);
                R_THROW(ams::fs::ResultUnsupportedOperation())
            }

            virtual Result DoCleanDirectoryRecursively(const ams::fs::Path &path) {
                AMS_UNUSED(path);
                R_THROW(ams::fs::ResultUnsupportedOperation())
            }

            virtual Result DoGetFileTimeStampRaw(ams::fs::FileTimeStampRaw *out, const ams::fs::Path &path) {
                R_SUCCEED_IF(R_SUCCEEDED(m_fs_1.GetFileTimeStampRaw(out, path)));
                R_RETURN(m_fs_2.GetFileTimeStampRaw(out, path));
            }
    };

    /*
     * Copy-on-Write / Read-Only Fallback ContentStorage FileSystem for cowMMC:
     *   - Primary filesystem (m_primary_fs): Read-Write emuMMC SD ContentStorage (/emuMMC/COW0/Nintendo/Contents).
     *   - Fallback filesystem (m_fallback_fs): Strictly Read-Only OFW SD ContentStorage (/Nintendo/Contents).
     * All mutations (Create/Delete/Rename/Clean/Commit/QueryEntry) target m_primary_fs exclusively.
     * Read and lookup operations query m_primary_fs first and fall back to m_fallback_fs on miss.
     */
    class CowLayeredContentStorageFileSystem : public ams::fs::fsa::IFileSystem {
        private:
            std::unique_ptr<ams::fs::fsa::IFileSystem> m_primary_fs;
            ams::fs::ReadOnlyFileSystem m_fallback_fs;
        public:
            explicit CowLayeredContentStorageFileSystem(std::unique_ptr<ams::fs::fsa::IFileSystem> primary, std::unique_ptr<ams::fs::fsa::IFileSystem> fallback)
                : m_primary_fs(std::move(primary)), m_fallback_fs(std::move(fallback)) { /* ... */ }

            virtual ~CowLayeredContentStorageFileSystem() { /* ... */ }
        private:
            virtual Result DoCreateFile(const ams::fs::Path &path, s64 size, int flags) override final {
                R_RETURN(m_primary_fs->CreateFile(path, size, flags));
            }

            virtual Result DoDeleteFile(const ams::fs::Path &path) override final {
                R_RETURN(m_primary_fs->DeleteFile(path));
            }

            virtual Result DoCreateDirectory(const ams::fs::Path &path) override final {
                R_RETURN(m_primary_fs->CreateDirectory(path));
            }

            virtual Result DoDeleteDirectory(const ams::fs::Path &path) override final {
                R_RETURN(m_primary_fs->DeleteDirectory(path));
            }

            virtual Result DoDeleteDirectoryRecursively(const ams::fs::Path &path) override final {
                R_RETURN(m_primary_fs->DeleteDirectoryRecursively(path));
            }

            virtual Result DoRenameFile(const ams::fs::Path &old_path, const ams::fs::Path &new_path) override final {
                R_RETURN(m_primary_fs->RenameFile(old_path, new_path));
            }

            virtual Result DoRenameDirectory(const ams::fs::Path &old_path, const ams::fs::Path &new_path) override final {
                R_RETURN(m_primary_fs->RenameDirectory(old_path, new_path));
            }

            virtual Result DoGetEntryType(ams::fs::DirectoryEntryType *out, const ams::fs::Path &path) override final {
                R_SUCCEED_IF(R_SUCCEEDED(m_primary_fs->GetEntryType(out, path)));
                R_RETURN(m_fallback_fs.GetEntryType(out, path));
            }

            virtual Result DoOpenFile(std::unique_ptr<ams::fs::fsa::IFile> *out_file, const ams::fs::Path &path, ams::fs::OpenMode mode) override final {
                R_SUCCEED_IF(R_SUCCEEDED(m_primary_fs->OpenFile(out_file, path, mode)));
                if ((mode & ams::fs::OpenMode_All) == ams::fs::OpenMode_Read) {
                    R_RETURN(m_fallback_fs.OpenFile(out_file, path, mode));
                }
                R_RETURN(m_primary_fs->OpenFile(out_file, path, mode));
            }

            virtual Result DoOpenDirectory(std::unique_ptr<ams::fs::fsa::IDirectory> *out_dir, const ams::fs::Path &path, ams::fs::OpenDirectoryMode mode) override final {
                R_SUCCEED_IF(R_SUCCEEDED(m_primary_fs->OpenDirectory(out_dir, path, mode)));
                R_RETURN(m_fallback_fs.OpenDirectory(out_dir, path, mode));
            }

            virtual Result DoCommit() override final {
                R_RETURN(m_primary_fs->Commit());
            }

            virtual Result DoGetFreeSpaceSize(s64 *out, const ams::fs::Path &path) override final {
                R_RETURN(m_primary_fs->GetFreeSpaceSize(out, path));
            }

            virtual Result DoGetTotalSpaceSize(s64 *out, const ams::fs::Path &path) override final {
                R_RETURN(m_primary_fs->GetTotalSpaceSize(out, path));
            }

            virtual Result DoCleanDirectoryRecursively(const ams::fs::Path &path) override final {
                R_RETURN(m_primary_fs->CleanDirectoryRecursively(path));
            }

            virtual Result DoGetFileTimeStampRaw(ams::fs::FileTimeStampRaw *out, const ams::fs::Path &path) override final {
                R_SUCCEED_IF(R_SUCCEEDED(m_primary_fs->GetFileTimeStampRaw(out, path)));
                R_RETURN(m_fallback_fs.GetFileTimeStampRaw(out, path));
            }

            virtual Result DoQueryEntry(char *dst, size_t dst_size, const char *src, size_t src_size, ams::fs::fsa::QueryId query, const ams::fs::Path &path) override final {
                R_RETURN(m_primary_fs->QueryEntry(dst, dst_size, src, src_size, query, path));
            }
    };

}
