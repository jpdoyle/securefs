#pragma once
#include "constants.h"
#include "exceptions.h"
#include "files.h"
#include "myutils.h"
#include "platform.h"
#include "streams.h"
#include "thread_pool.hpp"

#include <memory>
#include <string.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace securefs
{
class FileTableIO;

class AutoClosedFileBase;

class FileTable
{
    DISABLE_COPY_MOVE(FileTable)

private:
    typedef std::unordered_map<id_type, std::unique_ptr<FileBase>, id_hash> table_type;

private:
    static const int MAX_NUM_CLOSED = 201, NUM_EJECT = 150;

private:
    key_type m_master_key;
    table_type m_files;
    std::vector<id_type> m_closed_ids;

    thread_pool free_pool;
    table_type m_files_to_close;
    std::unordered_set<id_type,id_hash> m_closing_ids;
    std::mutex m_closing_lock;

    std::unique_ptr<FileTableIO> m_fio;
    uint32_t m_flags;
    unsigned m_block_size, m_iv_size;
    std::shared_ptr<const OSService> m_root;

private:
    void eject();
    void finalize(std::unique_ptr<FileBase>&);

public:
    explicit FileTable(int version,
                       std::shared_ptr<const OSService> root,
                       const key_type& master_key,
                       uint32_t flags,
                       unsigned block_size,
                       unsigned iv_size);
    ~FileTable();
    FileBase* open_as(const id_type& id, int type);
    FileBase* create_as(const id_type& id, int type);
    void close(FileBase*);
    bool is_readonly() const noexcept { return (m_flags & kOptionReadOnly) != 0; }
    bool is_auth_enabled() const noexcept { return (m_flags & kOptionNoAuthentication) == 0; }
    bool is_time_stored() const noexcept { return (m_flags & kOptionStoreTime) != 0; }
    void gc();
    void statfs(struct fuse_statvfs* fs_info) { m_root->statfs(fs_info); }
};

class AutoClosedFileBase
{
private:
    FileTable* m_ft;
    FileBase* m_fb;

public:
    explicit AutoClosedFileBase(FileTable* ft, FileBase* fb) : m_ft(ft), m_fb(fb) {}

    AutoClosedFileBase(const AutoClosedFileBase&) = delete;
    AutoClosedFileBase& operator=(const AutoClosedFileBase&) = delete;

    AutoClosedFileBase(AutoClosedFileBase&& other) noexcept : m_ft(other.m_ft), m_fb(other.m_fb)
    {
        other.m_ft = nullptr;
        other.m_fb = nullptr;
    }

    AutoClosedFileBase& operator=(AutoClosedFileBase&& other) noexcept
    {
        if (this == &other)
            return *this;
        swap(other);
        return *this;
    }

    ~AutoClosedFileBase()
    {
        try
        {
            reset(nullptr);
        }
        catch (...)
        {
        }
    }

    FileBase* get() noexcept { return m_fb; }
    template <class T>
    T* get_as() noexcept
    {
        return m_fb->cast_as<T>();
    }
    FileBase& operator*() noexcept { return *m_fb; }
    FileBase* operator->() noexcept { return m_fb; }
    FileBase* release() noexcept
    {
        auto rt = m_fb;
        m_fb = nullptr;
        return rt;
    }
    void reset(FileBase* fb)
    {
        if (m_ft && m_fb)
        {
            m_ft->close(m_fb);
        }
        m_fb = fb;
    }
    void swap(AutoClosedFileBase& other) noexcept
    {
        std::swap(m_ft, other.m_ft);
        std::swap(m_fb, other.m_fb);
    }
};

inline AutoClosedFileBase open_as(FileTable& table, const id_type& id, int type)
{
    return AutoClosedFileBase(&table, table.open_as(id, type));
}

inline AutoClosedFileBase create_as(FileTable& table, const id_type& id, int type)
{
    return AutoClosedFileBase(&table, table.create_as(id, type));
}
}    // namespace securefs
