#include "operations.h"
#include "case_fold.h"
#include "constants.h"
#include "crypto.h"
#include "platform.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <string>
#include <typeinfo>
#include <utility>

#ifdef __APPLE__
#include <sys/xattr.h>
#endif

namespace securefs
{
namespace operations
{
    const char* LOCK_FILENAME = ".securefs.lock";

    MountOptions::MountOptions() {}
    MountOptions::~MountOptions() {}

    FileSystemContext::FileSystemContext(const MountOptions& opt)
        : table(opt.version.value(),
                opt.root,
                from_cryptopp_key(opt.master_key),
                opt.flags.value(),
                opt.block_size.value(),
                opt.iv_size.value())
        , root(opt.root)
        , root_id()
        , flags(opt.flags.value())
    {
        if (opt.version.value() > 3)
            throwInvalidArgumentException("This context object only works with format 1,2,3");
        block_size = opt.block_size.value();
    }

    FileSystemContext::~FileSystemContext() {}

    bool is_prefix(const std::string& a,const std::string& b) {
        auto a_it = a.begin();
        auto b_it = b.begin();
        while(a_it != a.end() && b_it != b.end() && *a_it == *b_it) {
            ++a_it;
            ++b_it;
        }
        return a_it == a.end();
    }

    void FileSystemContext::clear_cache(id_type id) {
        auto it = id_reverse.find(id);
        if(it != id_reverse.end()) {
            clear_cache(it->second);
        }
    }

    void FileSystemContext::clear_cache(std::string path) {
        auto it = id_cache.find(path);
        // since `map` traverses in sorted order, this will catch all
        // subdirectories in the cache.
        while(it != id_cache.end() && is_prefix(path,it->first)) {
            id_reverse.erase(it->second);
            auto prev_it = it;
            ++it;
            id_cache.erase(prev_it);
        }
    }
}    // namespace operations
}    // namespace securefs

using securefs::operations::FileSystemContext;

namespace securefs
{
namespace internal
{
    inline FileSystemContext* get_fs(struct fuse_context* ctx)
    {
        return static_cast<FileSystemContext*>(ctx->private_data);
    }

    typedef AutoClosedFileBase FileGuard;

    FileGuard open_base_dir(FileSystemContext* fs, const char* path, std::string& last_component)
    {
        std::vector<std::string> components
            = split((fs->flags & kOptionCaseFoldFileName) ? case_fold(path) : path, '/');
        std::vector<std::string> prefixes;
        {
            std::string prefix = "";
            for(auto s: components) {
                prefix += "/" + s;
                prefixes.push_back(prefix);
            }
        }

        if (components.empty())
        {
            FileGuard result(&fs->table, fs->table.open_as(fs->root_id, FileBase::DIRECTORY));
            last_component = std::string();
            return result;
        }
        id_type id = fs->root_id;
        int type;

        size_t first_component = 0;

        auto cache_it = fs->id_cache.end();

        while(first_component + 1 < components.size() &&
              (cache_it =
               fs->id_cache.find(prefixes[first_component])) != fs->id_cache.end()) {
            id = cache_it->second;
            ++first_component;
        }

        FileGuard result(&fs->table, fs->table.open_as(id, FileBase::DIRECTORY));

        for (size_t i = first_component; i + 1 < components.size(); ++i)
        {
            bool exists = result.get_as<Directory>()->get_entry(components[i], id, type);
            if (!exists)
                throwVFSException(ENOENT);
            if (type != FileBase::DIRECTORY)
                throwVFSException(ENOTDIR);
            result.reset(fs->table.open_as(id, type));
            fs->id_cache[prefixes[i]] = id;
            fs->id_reverse[id] = prefixes[i];
        }
        last_component = components.back();
        return result;
    }

    FileGuard open_all(FileSystemContext* fs, const char* path)
    {
        std::string last_component;
        auto fg = open_base_dir(fs, path, last_component);
        if (last_component.empty())
            return fg;
        id_type id;
        int type;
        bool exists = fg.get_as<Directory>()->get_entry(last_component, id, type);
        if (!exists)
            throwVFSException(ENOENT);
        fg.reset(fs->table.open_as(id, type));
        return fg;
    }

    // Specialization of `open_all` since `VFSException(ENOENT)` occurs too frequently
    bool open_all(FileSystemContext* fs, const char* path, FileGuard& fg)
    {
        std::string last_component;
        fg = open_base_dir(fs, path, last_component);
        if (last_component.empty())
            return true;
        id_type id;
        int type;
        bool exists = fg.get_as<Directory>()->get_entry(last_component, id, type);
        if (!exists)
        {
            return false;
        }
        fg.reset(fs->table.open_as(id, type));
        return true;
    }

    FileGuard create(FileSystemContext* fs,
                     const char* path,
                     int type,
                     uint32_t mode,
                     uint32_t uid,
                     uint32_t gid)
    {
        std::string last_component;
        auto dir = open_base_dir(fs, path, last_component);
        id_type id;
        generate_random(id.data(), id.size());

        FileGuard result(&fs->table, fs->table.create_as(id, type));
        result->initialize_empty(mode, uid, gid);

        try
        {
            bool success = dir.get_as<Directory>()->add_entry(last_component, id, type);
            if (!success)
                throwVFSException(EEXIST);
        }
        catch (...)
        {
            result->unlink();
            throw;
        }
        return result;
    }

    void remove(FileSystemContext* fs, const id_type& id, int type)
    {
        try
        {
            FileGuard to_be_removed(&fs->table, fs->table.open_as(id, type));
            to_be_removed->unlink();
            fs->clear_cache(id);
        }
        catch (...)
        {
            // Errors in unlinking the actual underlying file can be ignored
            // They will not affect the apparent filesystem operations
        }
    }

    void remove(FileSystemContext* fs, const char* path)
    {
        std::string last_component;
        auto dir_guard = open_base_dir(fs, path, last_component);
        auto dir = dir_guard.get_as<Directory>();
        if (last_component.empty())
            throwVFSException(EPERM);
        id_type id;
        int type;
        if (!dir->get_entry(last_component, id, type))
            throwVFSException(ENOENT);

        FileGuard inner_guard = open_as(fs->table, id, type);
        auto inner_fb = inner_guard.get();
        if (inner_fb->type() == FileBase::DIRECTORY && !static_cast<Directory*>(inner_fb)->empty())
        {
            std::string contents;
            static_cast<Directory*>(inner_fb)->iterate_over_entries(
                [&contents](const std::string& str, const id_type&, int) -> bool {
                    contents.push_back('\n');
                    contents += str;
                    return true;
                });
            WARN_LOG("Trying to remove a non-empty directory \"%s\" with contents: %s",
                     path,
                     contents.c_str());
            throwVFSException(ENOTEMPTY);
        }
        dir->remove_entry(last_component, id, type);
        inner_fb->unlink();
    }

    inline bool is_readonly(struct fuse_context* ctx) { return get_fs(ctx)->table.is_readonly(); }
}    // namespace internal

namespace operations
{
#define COMMON_PROLOGUE                                                                            \
    auto ctx = fuse_get_context();                                                                 \
    auto fs = internal::get_fs(ctx);                                                               \
    (void)fs;                                                                                      \
    OPT_TRACE_WITH_PATH;

#define COMMON_CATCH_BLOCK OPT_CATCH_WITH_PATH

    void* init(struct fuse_conn_info* fsinfo)
    {
#ifdef FUSE_CAP_BIG_WRITES
        fsinfo->want |= FUSE_CAP_BIG_WRITES;
        fsinfo->max_write = static_cast<unsigned>(-1);
#endif
        auto args = static_cast<MountOptions*>(fuse_get_context()->private_data);
        auto fs = new FileSystemContext(*args);
        TRACE_LOG("%s", __FUNCTION__);
        fputs("Filesystem mounted successfully\n", stderr);
        return fs;
    }

    void destroy(void* data)
    {
        auto fs = static_cast<FileSystemContext*>(data);
        TRACE_LOG("%s", __FUNCTION__);
        delete fs;
        fputs("Filesystem unmounted successfully\n", stderr);
    }

    int statfs(const char* path, struct fuse_statvfs* fs_info)
    {
        COMMON_PROLOGUE
        try
        {
            fs->table.statfs(fs_info);
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int getattr(const char* path, struct fuse_stat* st)
    {
        COMMON_PROLOGUE

        try
        {
            if (!st)
                return -EINVAL;

            internal::FileGuard fg(nullptr, nullptr);
            if (!internal::open_all(fs, path, fg))
                return -ENOENT;
            fg->stat(st);
            st->st_uid = OSService::getuid();
            st->st_gid = OSService::getgid();
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int opendir(const char* path, struct fuse_file_info* info)
    {
        COMMON_PROLOGUE

        try
        {
            auto fg = internal::open_all(fs, path);
            if (fg->type() != FileBase::DIRECTORY)
                return -ENOTDIR;
            info->fh = reinterpret_cast<uintptr_t>(fg.release());

            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int releasedir(const char* path, struct fuse_file_info* info)
    {
        return ::securefs::operations::release(path, info);
    }

    int readdir(const char* path,
                void* buffer,
                fuse_fill_dir_t filler,
                fuse_off_t,
                struct fuse_file_info* info)
    {
        COMMON_PROLOGUE

        try
        {
            auto fb = reinterpret_cast<FileBase*>(info->fh);
            if (!fb)
                return -EFAULT;
            if (fb->type() != FileBase::DIRECTORY)
                return -ENOTDIR;
            struct fuse_stat st;
            memset(&st, 0, sizeof(st));
            auto actions
                = [&st, filler, buffer](const std::string& name, const id_type&, int type) -> bool {
                st.st_mode = FileBase::mode_for_type(type);
                bool success = filler(buffer, name.c_str(), &st, 0) == 0;
                if (!success)
                {
                    WARN_LOG("Filling directory buffer failed");
                }
                return success;
            };
            fb->cast_as<Directory>()->iterate_over_entries(actions);
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int create(const char* path, fuse_mode_t mode, struct fuse_file_info* info)
    {
        COMMON_PROLOGUE

        mode &= ~static_cast<uint32_t>(S_IFMT);
        mode |= S_IFREG;
        try
        {
            if (internal::is_readonly(ctx))
                return -EROFS;
            auto fg = internal::create(fs, path, FileBase::REGULAR_FILE, mode, ctx->uid, ctx->gid);
            fg->cast_as<RegularFile>();
            info->fh = reinterpret_cast<uintptr_t>(fg.release());

            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int open(const char* path, struct fuse_file_info* info)
    {
        COMMON_PROLOGUE

        int rdwr = info->flags & O_RDWR;
        int wronly = info->flags & O_WRONLY;
        int append = info->flags & O_APPEND;
        int require_write = wronly | append | rdwr;

        try
        {
            if (require_write && internal::is_readonly(ctx))
                return -EROFS;
            auto fg = internal::open_all(fs, path);
            RegularFile* file = fg->cast_as<RegularFile>();
            if (info->flags & O_TRUNC)
            {
                file->truncate(0);
            }
            info->fh = reinterpret_cast<uintptr_t>(fg.release());

            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int release(const char* path, struct fuse_file_info* info)
    {
        COMMON_PROLOGUE

        try
        {
            auto fb = reinterpret_cast<FileBase*>(info->fh);
            if (!fb)
                return -EINVAL;
            fb->flush();
            internal::FileGuard fg(&internal::get_fs(ctx)->table, fb);
            fg.reset(nullptr);
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int
    read(const char* path, char* buffer, size_t len, fuse_off_t off, struct fuse_file_info* info)
    {
        OPT_TRACE_WITH_PATH_OFF_LEN(off, len);

        try
        {
            auto fb = reinterpret_cast<FileBase*>(info->fh);
            if (!fb)
                return -EFAULT;
            return static_cast<int>(fb->cast_as<RegularFile>()->read(buffer, off, len));
        }
        OPT_CATCH_WITH_PATH_OFF_LEN(off, len)
    }

    int write(const char* path,
              const char* buffer,
              size_t len,
              fuse_off_t off,
              struct fuse_file_info* info)
    {
        OPT_TRACE_WITH_PATH_OFF_LEN(off, len);
        try
        {
            auto fb = reinterpret_cast<FileBase*>(info->fh);
            if (!fb)
                return -EFAULT;
            fb->cast_as<RegularFile>()->write(buffer, off, len);
            return static_cast<int>(len);
        }
        OPT_CATCH_WITH_PATH_OFF_LEN(off, len)
    }

    int flush(const char* path, struct fuse_file_info* info)
    {
        COMMON_PROLOGUE

        try
        {
            auto fb = reinterpret_cast<FileBase*>(info->fh);
            if (!fb)
                return -EFAULT;
            fb->cast_as<RegularFile>()->flush();
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int truncate(const char* path, fuse_off_t size)
    {
        COMMON_PROLOGUE

        try
        {
            auto fg = internal::open_all(fs, path);
            fg.get_as<RegularFile>()->truncate(size);
            fg->flush();
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int ftruncate(const char* path, fuse_off_t size, struct fuse_file_info* info)
    {
        COMMON_PROLOGUE

        try
        {
            auto fb = reinterpret_cast<FileBase*>(info->fh);
            if (!fb)
                return -EFAULT;
            fb->cast_as<RegularFile>()->truncate(size);
            fb->flush();
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int unlink(const char* path)
    {
        COMMON_PROLOGUE

        try
        {
            if (internal::is_readonly(ctx))
                return -EROFS;
            internal::remove(fs, path);
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int mkdir(const char* path, fuse_mode_t mode)
    {
        COMMON_PROLOGUE

        mode &= ~static_cast<uint32_t>(S_IFMT);
        mode |= S_IFDIR;
        try
        {
            if (internal::is_readonly(ctx))
                return -EROFS;
            auto fg = internal::create(fs, path, FileBase::DIRECTORY, mode, ctx->uid, ctx->gid);
            fg->cast_as<Directory>();
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int rmdir(const char* path) { return ::securefs::operations::unlink(path); }

    int chmod(const char* path, fuse_mode_t mode)
    {
        COMMON_PROLOGUE

        try
        {
            auto fg = internal::open_all(fs, path);
            auto original_mode = fg->get_mode();
            mode &= 0777;
            mode |= original_mode & S_IFMT;
            fg->set_mode(mode);
            fg->flush();
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int chown(const char* path, fuse_uid_t uid, fuse_gid_t gid)
    {
        COMMON_PROLOGUE

        try
        {
            auto fg = internal::open_all(fs, path);
            fg->set_uid(uid);
            fg->set_gid(gid);
            fg->flush();
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int symlink(const char* to, const char* from)
    {
        auto ctx = fuse_get_context();
        auto fs = internal::get_fs(ctx);
        OPT_TRACE_WITH_TWO_PATHS(to, from);

        try
        {
            if (internal::is_readonly(ctx))
                return -EROFS;
            auto fg
                = internal::create(fs, from, FileBase::SYMLINK, S_IFLNK | 0755, ctx->uid, ctx->gid);
            fg.get_as<Symlink>()->set(to);
            return 0;
        }
        OPT_CATCH_WITH_TWO_PATHS(to, from)
    }

    int readlink(const char* path, char* buf, size_t size)
    {
        if (size == 0)
            return -EINVAL;
        COMMON_PROLOGUE

        try
        {
            auto fg = internal::open_all(fs, path);
            auto destination = fg.get_as<Symlink>()->get();
            memset(buf, 0, size);
            memcpy(buf, destination.data(), std::min(destination.size(), size - 1));
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int rename(const char* src, const char* dst)
    {
        auto ctx = fuse_get_context();
        auto fs = internal::get_fs(ctx);
        OPT_TRACE_WITH_TWO_PATHS(src, dst);

        try
        {
            std::string src_filename, dst_filename;
            auto src_dir_guard = internal::open_base_dir(fs, src, src_filename);
            auto dst_dir_guard = internal::open_base_dir(fs, dst, dst_filename);
            auto src_dir = src_dir_guard.get_as<Directory>();
            auto dst_dir = dst_dir_guard.get_as<Directory>();

            id_type src_id, dst_id;
            int src_type, dst_type;

            if (!src_dir->get_entry(src_filename, src_id, src_type))
                return -ENOENT;
            bool dst_exists = (dst_dir->get_entry(dst_filename, dst_id, dst_type));

            if (dst_exists)
            {
                if (src_id == dst_id)
                    return 0;
                if (src_type != FileBase::DIRECTORY && dst_type == FileBase::DIRECTORY)
                    return -EISDIR;
                if (src_type != dst_type)
                    return -EINVAL;
                dst_dir->remove_entry(dst_filename, dst_id, dst_type);
            }
            src_dir->remove_entry(src_filename, src_id, src_type);
            dst_dir->add_entry(dst_filename, src_id, src_type);

            if (dst_exists)
                internal::remove(fs, dst_id, dst_type);

            fs->clear_cache(src_filename);

            return 0;
        }
        OPT_CATCH_WITH_TWO_PATHS(src, dst)
    }

    int link(const char* src, const char* dst)
    {
        auto ctx = fuse_get_context();
        auto fs = internal::get_fs(ctx);
        OPT_TRACE_WITH_TWO_PATHS(src, dst);

        try
        {
            std::string src_filename, dst_filename;
            auto src_dir_guard = internal::open_base_dir(fs, src, src_filename);
            auto dst_dir_guard = internal::open_base_dir(fs, dst, dst_filename);
            auto src_dir = src_dir_guard.get_as<Directory>();
            auto dst_dir = dst_dir_guard.get_as<Directory>();

            id_type src_id, dst_id;
            int src_type, dst_type;

            bool src_exists = src_dir->get_entry(src_filename, src_id, src_type);
            if (!src_exists)
                return -ENOENT;
            bool dst_exists = dst_dir->get_entry(dst_filename, dst_id, dst_type);
            if (dst_exists)
                return -EEXIST;

            auto&& table = internal::get_fs(ctx)->table;
            internal::FileGuard guard(&table, table.open_as(src_id, src_type));

            if (guard->type() != FileBase::REGULAR_FILE)
                return -EPERM;

            guard->set_nlink(guard->get_nlink() + 1);
            dst_dir->add_entry(dst_filename, src_id, src_type);
            return 0;
        }
        OPT_CATCH_WITH_TWO_PATHS(src, dst)
    }

    int fsync(const char* path, int, struct fuse_file_info* fi)
    {
        COMMON_PROLOGUE

        try
        {
            auto fb = reinterpret_cast<FileBase*>(fi->fh);
            if (!fb)
                return -EFAULT;
            fb->flush();
            fb->fsync();
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

    int fsyncdir(const char* path, int isdatasync, struct fuse_file_info* fi)
    {
        return ::securefs::operations::fsync(path, isdatasync, fi);
    }

    int utimens(const char* path, const struct fuse_timespec ts[2])
    {
        COMMON_PROLOGUE

        try
        {
            auto fg = internal::open_all(fs, path);
            fg->utimens(ts);
            return 0;
        }
        COMMON_CATCH_BLOCK
    }

#ifdef __APPLE__

    int listxattr(const char* path, char* list, size_t size)
    {
        COMMON_PROLOGUE

        try
        {
            auto fg = internal::open_all(fs, path);
            return static_cast<int>(fg->listxattr(list, size));
        }
        COMMON_CATCH_BLOCK
    }

    static const char* APPLE_FINDER_INFO = "com.apple.FinderInfo";

#define XATTR_COMMON_PROLOGUE                                                                      \
    auto ctx = fuse_get_context();                                                                 \
    auto fs = internal::get_fs(ctx);                                                               \
    TRACE_LOG("%s (path=%s, name=%s)", __func__, path, name);

#define XATTR_COMMON_CATCH_BLOCK                                                                   \
    catch (const std::exception& e)                                                                \
    {                                                                                              \
        auto ebase = dynamic_cast<const ExceptionBase*>(&e);                                       \
        int errc = ebase ? ebase->error_number() : EPERM;                                          \
        if (errc != ENOATTR) /* Attribute not found is very common and normal; no need to log it   \
                                as an error */                                                     \
            ERROR_LOG("%s (path=%s, name=%s) encounters %s: %s",                                   \
                      __FUNCTION__,                                                                \
                      path,                                                                        \
                      name,                                                                        \
                      get_type_name(e).get(),                                                      \
                      e.what());                                                                   \
        return -errc;                                                                              \
    }

    int getxattr(const char* path, const char* name, char* value, size_t size, uint32_t position)
    {
        XATTR_COMMON_PROLOGUE

        if (position != 0)
            return -EINVAL;

        try
        {
            auto fg = internal::open_all(fs, path);
            return static_cast<int>(fg->getxattr(name, value, size));
        }
        XATTR_COMMON_CATCH_BLOCK
    }

    int setxattr(const char* path,
                 const char* name,
                 const char* value,
                 size_t size,
                 int flags,
                 uint32_t position)
    {
        XATTR_COMMON_PROLOGUE

        if (position != 0)
            return -EINVAL;
        if (strcmp(name, "com.apple.quarantine") == 0)
            return 0;    // workaround for the "XXX is damaged" bug on OS X
        if (strcmp(name, APPLE_FINDER_INFO) == 0)
            return -EACCES;

        flags &= XATTR_CREATE | XATTR_REPLACE;
        try
        {
            auto fg = internal::open_all(fs, path);
            fg->setxattr(name, value, size, flags);
            return 0;
        }
        XATTR_COMMON_CATCH_BLOCK
    }

    int removexattr(const char* path, const char* name)
    {
        XATTR_COMMON_PROLOGUE

        try
        {
            auto fg = internal::open_all(fs, path);
            fg->removexattr(name);
            return 0;
        }
        XATTR_COMMON_CATCH_BLOCK
    }
#endif

    void init_fuse_operations(struct fuse_operations* opt, bool xattr)
    {
        memset(opt, 0, sizeof(*opt));
        opt->getattr = &securefs::operations::getattr;
        opt->init = &securefs::operations::init;
        opt->destroy = &securefs::operations::destroy;
        opt->opendir = &securefs::operations::opendir;
        opt->releasedir = &securefs::operations::releasedir;
        opt->readdir = &securefs::operations::readdir;
        opt->create = &securefs::operations::create;
        opt->open = &securefs::operations::open;
        opt->read = &securefs::operations::read;
        opt->write = &securefs::operations::write;
        opt->truncate = &securefs::operations::truncate;
        opt->unlink = &securefs::operations::unlink;
        opt->mkdir = &securefs::operations::mkdir;
        opt->rmdir = &securefs::operations::rmdir;
        opt->release = &securefs::operations::release;
        opt->ftruncate = &securefs::operations::ftruncate;
        opt->flush = &securefs::operations::flush;
        opt->chmod = &securefs::operations::chmod;
        opt->chown = &securefs::operations::chown;
        opt->symlink = &securefs::operations::symlink;
        opt->readlink = &securefs::operations::readlink;
        opt->rename = &securefs::operations::rename;
        opt->link = &securefs::operations::link;
        opt->fsync = &securefs::operations::fsync;
        opt->fsyncdir = &securefs::operations::fsyncdir;
        opt->utimens = &securefs::operations::utimens;
        opt->statfs = &securefs::operations::statfs;

        if (!xattr)
            return;
#ifdef __APPLE__
        opt->listxattr = &securefs::operations::listxattr;
        opt->getxattr = &securefs::operations::getxattr;
        opt->setxattr = &securefs::operations::setxattr;
        opt->removexattr = &securefs::operations::removexattr;
#endif
    }
}    // namespace operations
}    // namespace securefs
