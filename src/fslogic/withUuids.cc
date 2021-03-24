/**
 * @file withUuids.cc
 * @author Konrad Zemek
 * @copyright (C) 2016 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#include "withUuids.h"

namespace one {
namespace client {
namespace fslogic {
namespace detail {

struct stat toStatbuf(const FileAttrPtr &attr, const fuse_ino_t ino)
{
    struct stat statbuf = {0};

    constexpr size_t kBlockSize = 4096;
    constexpr size_t kStatBlockSize = 512;

    statbuf.st_atime = std::chrono::system_clock::to_time_t(attr->atime());
    statbuf.st_mtime = std::chrono::system_clock::to_time_t(attr->mtime());
    statbuf.st_ctime = std::chrono::system_clock::to_time_t(attr->ctime());
    statbuf.st_gid = attr->gid();
    statbuf.st_uid = attr->uid();
    statbuf.st_mode = attr->mode();
    statbuf.st_size = attr->size() ? *attr->size() : 0;
    statbuf.st_nlink = 1;
    statbuf.st_blksize = kBlockSize;
    // The block count must be returned in 512B blocks, i.e. an eigth
    // of the 4K block size
    statbuf.st_blocks = std::ceil(static_cast<double>(statbuf.st_size) /
        static_cast<double>(kStatBlockSize));
    statbuf.st_ino = ino;

    switch (attr->type()) {
        case messages::fuse::FileAttr::FileType::directory:
            statbuf.st_mode |= S_IFDIR;
            // Remove sticky bit for nfs compatibility
            statbuf.st_mode &= ~S_ISVTX;
            break;
        case messages::fuse::FileAttr::FileType::symlink:
            statbuf.st_mode |= S_IFLNK;
            break;
        case messages::fuse::FileAttr::FileType::link:
        case messages::fuse::FileAttr::FileType::regular:
            statbuf.st_mode |= S_IFREG;
            break;
    }

    return statbuf;
}

} // namespace detail
} // namespace fslogic
} // namespace client
} // namespace one
