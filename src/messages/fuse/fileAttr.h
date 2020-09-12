/**
 * @file fileAttr.h
 * @author Konrad Zemek
 * @copyright (C) 2015 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#ifndef ONECLIENT_MESSAGES_FUSE_FILE_ATTR_H
#define ONECLIENT_MESSAGES_FUSE_FILE_ATTR_H

#include "events/types/event.h"
#include "fuseResponse.h"

#include "messages.pb.h"

#include <folly/FBString.h>
#include <folly/Optional.h>

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace one {
namespace client {
namespace virtualfs {
class VirtualFsAdapter;
}
}
namespace messages {
namespace fuse {

using one::client::virtualfs::VirtualFsAdapter;

/**
 * The FileAttr class represents server-sent attributes of a file.
 */
class FileAttr : public FuseResponse {
public:
    using ProtocolMessage = clproto::FileAttr;

    enum class FileType { regular, directory, link };

    FileAttr() = default;
    FileAttr(const FileAttr &) = default;

    /**
     * Constructor.
     * @param message Protocol Buffers message that wraps @c
     * one::clproto::FileAttr message.
     */
    FileAttr(std::unique_ptr<ProtocolServerMessage> serverMessage);

    /**
     * Constructor.
     * @param message Protocol Buffers message representing @c FileAttr
     * counterpart.
     */
    FileAttr(const ProtocolMessage &message);

    /**
     * @return UUID of the file.
     */
    const folly::fbstring &uuid() const;

    /**
     * Sets new UUID of the file
     * @param uuid The UUID to set.
     */
    void setUuid(folly::fbstring uuid);

    /**
     * @returns Name of the file.
     */
    const folly::fbstring &name() const { return m_name; };

    /**
     * Sets new name of the file.
     * @param name Name to set.
     */
    void setName(folly::fbstring name) { m_name.swap(name); }

    /**
     * @returns UUID of the file's parent.
     */
    const folly::Optional<folly::fbstring> &parentUuid() const
    {
        return m_parentUuid;
    };

    /**
     * Sets new UUID of the file's parent.
     * @param parentUuid Uuid to set.
     */
    void setParentUuid(folly::fbstring parentUuid)
    {
        m_parentUuid = std::move(parentUuid);
    }

    /**
     * @return File access mode.
     */
    mode_t mode() const;

    /**
     * Sets a new mode.
     * @param mode The mode to set.
     */
    void mode(const mode_t mode);

    /**
     * @return ID of the file's owner.
     */
    uid_t uid() const;

    /**
     * Sets a new uid.
     * @param uid The uid to set.
     */
    void uid(const uid_t uid);

    /**
     * @return Group ID of the file's owner.
     */
    gid_t gid() const;

    /**
     * Sets a new gid.
     * @param gid The gid to set.
     */
    void gid(const gid_t gid);

    /**
     * @return Last access time to the file.
     */
    std::chrono::system_clock::time_point atime() const;

    /**
     * Set file's last access time.
     * @param time The access time to set.
     */
    void atime(std::chrono::system_clock::time_point time);

    /**
     * @return Last modification time of the file.
     */
    std::chrono::system_clock::time_point mtime() const;

    /**
     * Set file's last modification time.
     * @param time The modification time to set.
     */
    void mtime(std::chrono::system_clock::time_point time);

    /**
     * @return File's change time.
     */
    std::chrono::system_clock::time_point ctime() const;

    /**
     * Set file's change time.
     * @param time The change time to set.
     */
    void ctime(std::chrono::system_clock::time_point time);

    /**
     * @return Type of the file (regular, link, directory).
     */
    FileType type() const;

    void setType(FileType fileType) { m_type = fileType; }

    /**
     * @return Size of the file.
     */
    folly::Optional<off_t> size() const;

    /**
     * Set file size.
     */
    void size(const off_t size);

    /**
     * @brief Serialize FileAttr to string
     *
     * @return String representation of the FileAttr.
     */
    std::string toString() const override;

    /**
     * Returns true if the FileAttr represents a virtual file
     */
    bool isVirtual() const;

    /**
     * Returns true if the FileAttr represents a directory
     * which is an entrypoint to a virtual subtree
     */
    bool isVirtualEntrypoint() const { return m_isVirtualEntrypoint; }

    /**
     * @brief Set virtual entrypoint flag
     *
     * @param ve true if the FileAttr represents a virtual fs entrypoint
     */
    void setVirtualEntrypoint(bool ve) { m_isVirtualEntrypoint = ve; }

    /**
     * @brief Set virtual fs adapter
     *
     * @param virtualFsAdapter Shared pointer to VirtualFSAdapter instance
     */
    void setVirtualFsAdapter(
        std::shared_ptr<VirtualFsAdapter> virtualFsAdapter);

    /**
     * @brief Get virtual fs adapter instance
     *
     * @return Virtual fs adapter instance
     */
    std::shared_ptr<VirtualFsAdapter> getVirtualFsAdapter() const;

private:
    void deserialize(const ProtocolMessage &message);

    folly::fbstring m_uuid;
    folly::fbstring m_name;
    folly::Optional<folly::fbstring> m_parentUuid;
    mode_t m_mode{};
    uid_t m_uid{};
    gid_t m_gid{};
    std::chrono::system_clock::time_point m_atime;
    std::chrono::system_clock::time_point m_mtime;
    std::chrono::system_clock::time_point m_ctime;
    FileType m_type;
    folly::Optional<off_t> m_size{};
    std::shared_ptr<VirtualFsAdapter> m_virtualFsAdapter{};
    bool m_isVirtualEntrypoint{false};
};

} // namespace fuse
} // namespace messages
} // namespace one

#endif // ONECLIENT_MESSAGES_FUSE_FILE_ATTR_H
