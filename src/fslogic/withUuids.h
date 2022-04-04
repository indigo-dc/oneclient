/**
 * @file withUuids.h
 * @author Konrad Zemek
 * @copyright (C) 2016 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#pragma once

#include "attrs.h"
#include "cache/inodeCache.h"
#include "helpers/logging.h"
#include "ioTraceLogger.h"
#include "messages/fuse/fileAttr.h"

#include <folly/FBString.h>
#include <folly/io/IOBufQueue.h>

#include <chrono>
#include <cstdint>
#include <functional>

namespace one {
namespace client {
namespace fslogic {

namespace detail {
struct stat toStatbuf(const FileAttrPtr &attr, const fuse_ino_t ino);
} // namespace detail

/**
 * @c WithUuids is responsible for translating inodes to uuids.
 */
template <typename FsLogicT> class WithUuids {
public:
    template <typename... Args>
    WithUuids(folly::fbstring rootUuid, Args &&...args)
        : m_inodeCache{std::move(rootUuid)}
        , m_generation{std::chrono::system_clock::to_time_t(
              std::chrono::system_clock::now())}
        , m_fsLogic{std::forward<Args>(args)...}
    {
        m_fsLogic.onMarkDeleted(std::bind(&cache::InodeCache::markDeleted,
            &m_inodeCache, std::placeholders::_1));

        m_fsLogic.onRename(std::bind(&cache::InodeCache::rename, &m_inodeCache,
            std::placeholders::_1, std::placeholders::_2));
    }

    auto lookup(const fuse_ino_t ino, const folly::fbstring &name)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(name);

        FileAttrPtr attr = wrap(&FsLogicT::lookup, ino, name);
        return toEntry(std::move(attr));
    }

    void forget(const fuse_ino_t ino, const std::size_t count)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(count);

        m_inodeCache.forget(ino, count);
    }

    auto getattr(const fuse_ino_t ino)
    {
        LOG_FCALL() << LOG_FARG(ino);

        FileAttrPtr attr = wrap(&FsLogicT::getattr, ino);
        return detail::toStatbuf(std::move(attr), ino);
    }

    auto opendir(const fuse_ino_t ino)
    {
        LOG_FCALL() << LOG_FARG(ino);

        return wrap(&FsLogicT::opendir, ino);
    }

    auto releasedir(const fuse_ino_t ino, const std::uint64_t handle)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(handle);

        return wrap(&FsLogicT::releasedir, ino, handle);
    }

    auto readdir(const fuse_ino_t ino, const size_t maxSize, const off_t off)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(maxSize) << LOG_FARG(off);

        return wrap(&FsLogicT::readdir, ino, maxSize, off);
    }

    auto open(const fuse_ino_t ino, const int flags)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(flags);

        return wrap(&FsLogicT::open, ino, flags, 0);
    }

    auto read(const fuse_ino_t ino, const std::uint64_t handle,
        const off_t offset, const std::size_t size)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(handle) << LOG_FARG(offset)
                    << LOG_FARG(size);

        return wrap(&FsLogicT::read, ino, handle, offset, size,
            folly::Optional<folly::fbstring>{}, FsLogicT::MAX_RETRY_COUNT,
            std::unique_ptr<IOTraceRead>{});
    }

    auto write(const fuse_ino_t ino, const std::uint64_t handle,
        const off_t offset, std::shared_ptr<folly::IOBuf> buf)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(handle) << LOG_FARG(offset)
                    << LOG_FARG(buf->length());

        return wrap(&FsLogicT::write, ino, handle, offset, std::move(buf),
            FsLogicT::MAX_RETRY_COUNT, std::unique_ptr<IOTraceWrite>{});
    }

    auto release(const fuse_ino_t ino, const std::uint64_t handle)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(handle);

        return wrap(&FsLogicT::release, ino, handle);
    }

    auto mkdir(
        const fuse_ino_t ino, const folly::fbstring &name, const mode_t mode)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(name) << LOG_FARG(mode);

        FileAttrPtr attr = wrap(&FsLogicT::mkdir, ino, name, mode);
        return toEntry(std::move(attr));
    }

    auto mknod(
        const fuse_ino_t ino, const folly::fbstring &name, const mode_t mode)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(name) << LOG_FARG(mode);

        FileAttrPtr attr = wrap(&FsLogicT::mknod, ino, name, mode);
        return toEntry(std::move(attr));
    }

    auto link(const fuse_ino_t ino, const fuse_ino_t newParent,
        const folly::fbstring &newName)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(newParent)
                    << LOG_FARG(newName);

        FileAttrPtr attr =
            wrap(&FsLogicT::link, ino, m_inodeCache.at(newParent), newName);
        return toEntry(std::move(attr));
    }

    auto symlink(const fuse_ino_t parent, const folly::fbstring &name,
        const folly::fbstring &link)
    {
        LOG_FCALL() << LOG_FARG(parent) << LOG_FARG(name) << LOG_FARG(link);

        FileAttrPtr attr = wrap(&FsLogicT::symlink, parent, name, link);
        return toEntry(std::move(attr));
    }

    auto readlink(const fuse_ino_t ino)
    {
        LOG_FCALL() << LOG_FARG(ino);

        folly::fbstring link = wrap(&FsLogicT::readlink, ino);

        return link;
    }

    auto unlink(const fuse_ino_t ino, const folly::fbstring &name)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(name);

        return wrap(&FsLogicT::unlink, ino, name);
    }

    auto rename(const fuse_ino_t ino, const folly::fbstring &name,
        const fuse_ino_t targetIno, const folly::fbstring &targetName)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(name) << LOG_FARG(targetIno)
                    << LOG_FARG(targetName);

        const auto targetUuid = m_inodeCache.at(targetIno);
        return wrap(&FsLogicT::rename, ino, name, targetUuid, targetName);
    }

    auto setattr(const fuse_ino_t ino, const struct stat &attr, const int toSet)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(attr.st_ino)
                    << LOG_FARG(toSet);

        FileAttrPtr ret = wrap(&FsLogicT::setattr, ino, attr, toSet);
        return detail::toStatbuf(std::move(ret), ino);
    }

    std::pair<struct fuse_entry_param, std::uint64_t> create(
        const fuse_ino_t ino, const folly::fbstring &name, const mode_t mode,
        const int flags)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(name) << LOG_FARGO(mode)
                    << LOG_FARG(flags);

        auto ret = wrap(&FsLogicT::create, ino, name, mode, flags);
        return {toEntry(std::move(ret.first)), ret.second};
    }

    auto statfs(const fuse_ino_t ino)
    {
        LOG_FCALL();

        auto statinfo = wrap(&FsLogicT::statfs, ino);
        statinfo.f_fsid = m_generation;
        return statinfo;
    }

    auto flush(const fuse_ino_t ino, const std::uint64_t handle)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(handle);

        return wrap(&FsLogicT::flush, ino, handle);
    }

    auto fsync(
        const fuse_ino_t ino, const std::uint64_t handle, const bool dataOnly)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(handle) << LOG_FARG(dataOnly);

        return wrap(&FsLogicT::fsync, ino, handle, dataOnly);
    }

    auto listxattr(const fuse_ino_t ino)
    {
        LOG_FCALL() << LOG_FARG(ino);

        return wrap(&FsLogicT::listxattr, ino);
    }

    auto getxattr(const fuse_ino_t ino, const folly::fbstring &name)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(name);

        return wrap(&FsLogicT::getxattr, ino, name);
    }

    auto setxattr(const fuse_ino_t ino, const folly::fbstring &name,
        const folly::fbstring &value, bool create, bool replace)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(name) << LOG_FARG(value)
                    << LOG_FARG(create) << LOG_FARG(replace);

        return wrap(&FsLogicT::setxattr, ino, name, value, create, replace);
    }

    auto removexattr(const fuse_ino_t ino, const folly::fbstring &name)
    {
        LOG_FCALL() << LOG_FARG(ino) << LOG_FARG(name);

        return wrap(&FsLogicT::removexattr, ino, name);
    }

    bool isFullBlockReadForced() const
    {
        return m_fsLogic.isFullBlockReadForced();
    }

private:
    template <typename Ret, typename... FunArgs, typename... Args>
    inline constexpr Ret wrap(
        Ret (FsLogicT::*fun)(const folly::fbstring &, FunArgs...),
        const fuse_ino_t inode, Args &&...args)
    {
        const auto &uuid = m_inodeCache.at(inode);
        return (m_fsLogic.*fun)(uuid, std::forward<Args>(args)...);
    }

    struct fuse_entry_param toEntry(const FileAttrPtr attr)
    {
        struct fuse_entry_param entry = {0};
        entry.generation = m_generation;
        entry.ino = m_inodeCache.lookup(attr->uuid());
        entry.attr = detail::toStatbuf(attr, entry.ino);

        return entry;
    }

    cache::InodeCache m_inodeCache;
    const long long m_generation;
    FsLogicT m_fsLogic;
};

} // namespace fslogic
} // namespace client
} // namespace one
