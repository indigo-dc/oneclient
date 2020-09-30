/**
 * @file fileLocation.h
 * @author Konrad Zemek
 * @copyright (C) 2015 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#ifndef ONECLIENT_MESSAGES_FUSE_FILE_LOCATION_H
#define ONECLIENT_MESSAGES_FUSE_FILE_LOCATION_H

#include "events/types/event.h"
#include "fileBlock.h"
#include "fuseResponse.h"

#include <boost/icl/interval_map.hpp>
#include <folly/FBString.h>

#include <sys/types.h>

#include <memory>
#include <set>
#include <string>
#include <unordered_map>

namespace one {
namespace clproto {
class FileLocation;
}
namespace messages {
namespace fuse {

using BlocksMap =
    std::map<folly::fbstring, folly::fbvector<std::pair<off_t, off_t>>>;

/**
 * The @c FileLocation class represents server-sent information about file
 * location.
 */
class FileLocation : public FuseResponse {
public:
    using FileBlocksMap = boost::icl::interval_map<off_t, FileBlock,
        boost::icl::partial_enricher>;
    using ProtocolMessage = clproto::FileLocation;

    FileLocation() = default;

    /**
     * Constructor.
     * @param message Protocol Buffers message that wraps @c
     * one::clproto::FileLocation message.
     */
    FileLocation(std::unique_ptr<ProtocolServerMessage> serverMessage);

    /**
     * Constructor.
     * @param message Protocol Buffers message representing @c FileLocation
     * counterpart.
     */
    FileLocation(const ProtocolMessage &message);

    /**
     * @return File UUID.
     */
    const std::string &uuid() const;
    void setUuid(const std::string &uuid) { m_uuid = uuid; }

    /**
     * @return ID of a space the file belongs to.
     */
    const std::string &spaceId() const;

    /**
     * @return Default storage ID of the file.
     */
    const std::string &storageId() const;

    /**
     * Set new storage id.
     * @param storageId The storage id to set.
     */
    void storageId(std::string storageId);

    /**
     * @return File ID on the default storage id.
     */
    const std::string &fileId() const;

    /**
     * Set new file id.
     * @param fileId The file id to set.
     */
    void fileId(std::string fileId);

    /**
     * Adds new file block to file location map.
     * @param offset Offset of the block in the file
     * @param size Length of the block
     * @param block File block
     */
    void putBlock(const off_t offset, const size_t size, FileBlock &&block);

    /**
     * Adds new file block to file location map using icl discrete interval.
     * @param range Interval for this block
     * @param block File block
     */
    void putBlock(
        const std::pair<boost::icl::discrete_interval<off_t>, FileBlock>
            &block);

    /**
     * Truncates the file location map to a given range.
     * @param range A discrete interval to which the existing blocks should be
     * truncated.
     */
    void truncate(const boost::icl::discrete_interval<off_t> &range);

    /**
     * Updates file location blocks only within entire range.
     * @param blocks File location from which the blocks in the given range
     *               should replace the current blocks in that range
     */
    void update(const FileBlocksMap &blocks);

    /**
     * Updates file location blocks only within a specified range using blocks
     * from another file location.
     * @param start Offset of the update range (inclusive)
     * @param end End of the update range (exclusive)
     * @param blocks File location from which the blocks in the given range
     *               should replace the current blocks in that range
     */
    void updateInRange(
        const off_t start, const off_t end, const FileLocation &blocks);

    /**
     * @return Blocks per storageId/fileId pair.
     */
    const FileBlocksMap &blocks() const;

    /**
     * @return Return the map of the blocks indexed by storage id.
     */
    BlocksMap getFileLocalBlocks() const;

    /**
     * Returns the number of separate blocks in the file location map.
     */
    unsigned int blocksCount() const;

    /**
     * @return Version of this location.
     */
    std::uint64_t version() const;

    /**
     * Sets a new location version.
     * @param version Version to set for the location.
     */
    void version(std::uint64_t);

    std::string toString() const override;

    /**
     * Render progress string of specified length for the file location map.
     * @param fileSize The current file size
     * @param progressSteps The length of the rendered progress bar
     * @return Block replication map
     */
    std::string progressString(
        const size_t fileSize, const size_t progressSteps) const;

    /**
     * Calculates replication progress with respect the provided file size.
     * @param fileSize The current file size
     * @return Replication progress [0.0-1.0]
     */
    double replicationProgress(const size_t fileSize) const;

    /**
     * Evaluates whether entire block range for the given file is available in
     * the file location map.
     * @param fileSize The current size of the file from the metadata
     */
    bool isReplicationComplete(const size_t fileSize) const;

    /**
     * Calculates the number of different blocks in a given range
     * @param start The start offset of the requested range (inclusive)
     * @param end The end offset of the range  (noninclusive)
     * @return Number of blocks in range [start, end)
     */
    unsigned int blocksInRange(const off_t start, const off_t end) const;

    /**
     * Calculates the length of blocks in a given range.
     * @param start The start offset of the requested range (inclusive)
     * @param end The end offset of the range  (noninclusive)
     * @return Number of blocks in range [start, end)
     */
    size_t blocksLengthInRange(const off_t start, const off_t end) const;

    /**
     * Determines whether the linear read prefetch threshold has been reached.
     */
    bool linearReadPrefetchThresholdReached(
        const double threshold, const size_t fileSize) const;

    /**
     * Determines whether the random read prefetch threshold has been reached.
     */
    bool randomReadPrefetchThresholdReached(
        const double threshold, const size_t fileSize) const;

private:
    void deserialize(const ProtocolMessage &message);
    void invalidateCachedValues();

    std::string m_uuid;
    std::string m_spaceId;
    std::string m_storageId;
    std::string m_fileId;
    FileBlocksMap m_blocks;
    std::uint64_t m_version;

    mutable bool m_replicationProgressCachedValid;
    mutable double m_replicationProgressCachedValue;

    mutable bool m_progressStringCachedValid;
    mutable std::string m_progressStringCachedValue;
};

} // namespace fuse
} // namespace messages
} // namespace one

#endif // ONECLIENT_MESSAGES_FUSE_FILE_LOCATION_H
