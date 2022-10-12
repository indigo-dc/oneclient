/**
 * @file fsLogic.h
 * @author Konrad Zemek
 * @copyright (C) 2016 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#pragma once

#include "fuseFileHandle.h"

#include "attrs.h"
#include "cache/forceProxyIOCache.h"
#include "cache/helpersCache.h"
#include "cache/openFileMetadataCache.h"
#include "cache/readdirCache.h"
#include "events/events.h"
#include "fsSubscriptions.h"
#include "fslogic/fiberBound.h"
#include "ioTraceLogger.h"
#include "virtualfs/virtualFsRegistry.h"

#include <boost/icl/discrete_interval.hpp>
#include <folly/FBString.h>
#include <folly/FBVector.h>
#include <folly/Function.h>
#include <folly/io/IOBufQueue.h>

#include <functional>
#include <memory>
#include <random>
#include <unordered_map>
#include <unordered_set>

constexpr auto ONE_XATTR_PREFIX = "org.onedata.";

namespace one {

namespace messages {
class Configuration;
namespace fuse {
class FileBlock;
class FuseResponse;
class SyncResponse;
} // namespace fuse
} // namespace messages

namespace client {

class Context;

namespace fslogic {

const std::array<std::pair<int, int>, 6> FSLOGIC_RETRY_DELAYS{
    {{4'000, 6'000}, {5'000, 10'000}, {10'000, 15'000}, {10'000, 20'000},
        {10'000, 30'000}, {10'000, 30'000}}};

constexpr int FSLOGIC_RETRY_COUNT = FSLOGIC_RETRY_DELAYS.size();

constexpr auto SYNCHRONIZE_BLOCK_PRIORITY_IMMEDIATE = 32;
constexpr auto SYNCHRONIZE_BLOCK_PRIORITY_LINEAR_PREFETCH = 96;
constexpr auto SYNCHRONIZE_BLOCK_PRIORITY_CLUSTER_PREFETCH = 160;

const auto ONEDATA_FILEID_ACCESS_PREFIX = ".__onedata__file_id__";

/**
 * The FsLogic main class.
 * This class contains FUSE all callbacks, so it basically is an heart of the
 * filesystem. Technically FsLogic is an singleton created on program start and
 * registered in FUSE daemon.
 */
class FsLogic : public FiberBound {
public:
    constexpr static int MAX_RETRY_COUNT = FSLOGIC_RETRY_COUNT;

    using BlocksMap =
        std::map<folly::fbstring, folly::fbvector<std::pair<off_t, off_t>>>;
    /**
     * Constructor.
     * @param context Shared pointer to application context instance.
     * @param configuration Starting configuration from server.
     * @param helpersCache Cache from which helpers will be fetched.
     * @param readEventsDisabled Specifies if FileRead event should be emitted.
     * @param providerTimeout Timeout for provider connection.
     * @param runInFiber A function that runs callback inside a main fiber.
     */
    FsLogic(std::shared_ptr<Context> context,
        std::shared_ptr<messages::Configuration> configuration,
        std::unique_ptr<cache::HelpersCache> helpersCache,
        unsigned int metadataCacheSize, bool readEventsDisabled,
        bool forceFullblockRead, const std::chrono::seconds providerTimeout,
        const std::chrono::seconds directoryCacheDropAfter,
        std::function<void(folly::Function<void()>)> runInFiber,
        bool autoStart = true);

    ~FsLogic();

    /**
     * Perform operations required to start FsLogic properly
     */
    void start();

    void stop();

    /**
     * Reset FsLogic state, e.g. after a connection loss.
     */
    void reset();

    /**
     * FUSE @c lookup callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    struct statvfs statfs(const folly::fbstring &uuid);

    /**
     * FUSE @c lookup callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    FileAttrPtr lookup(
        const folly::fbstring &uuid, const folly::fbstring &name);

    /**
     * FUSE @c getattr callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    FileAttrPtr getattr(const folly::fbstring &uuid);

    /**
     * FUSE @c opendir callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    std::uint64_t opendir(const folly::fbstring &uuid);

    /**
     * FUSE @c releasedir callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    void releasedir(
        const folly::fbstring &uuid, const std::uint64_t fileHandleId);

    /**
     * FUSE @c readdir callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    folly::fbvector<folly::fbstring> readdir(
        const folly::fbstring &uuid, const size_t maxSize, const off_t off);

    /**
     * FUSE @c open callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    std::uint64_t open(const folly::fbstring &uuid, const int flags,
        const std::uint64_t reuseFuseFileHandleId = 0);

    /**
     * FUSE @c release callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    void release(const folly::fbstring &uuid, const std::uint64_t fileHandleId);

    /**
     * FUSE @c read callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    folly::IOBufQueue read(const folly::fbstring &uuid,
        const std::uint64_t fileHandleId, const off_t offset,
        const std::size_t size,
        const folly::Optional<folly::fbstring> &checksum,
        const int retriesLeft = FSLOGIC_RETRY_COUNT,
        std::unique_ptr<IOTraceRead> ioTraceEntry = {});

    /**
     * FUSE @c write callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    std::size_t write(const folly::fbstring &uuid,
        const std::uint64_t fuseFileHandleId, const off_t offset,
        std::shared_ptr<folly::IOBuf> buf,
        const int retriesLeft = FSLOGIC_RETRY_COUNT,
        std::unique_ptr<IOTraceWrite> ioTraceEntry = {});

    /**
     * FUSE @c mkdir callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    FileAttrPtr mkdir(const folly::fbstring &parentUuid,
        const folly::fbstring &name, const mode_t mode);

    /**
     * FUSE @c mknod callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    FileAttrPtr mknod(const folly::fbstring &parentUuid,
        const folly::fbstring &name, const mode_t mode);

    /**
     * FUSE @c link callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    FileAttrPtr link(const folly::fbstring &uuid,
        const folly::fbstring &newParentUuid, const folly::fbstring &newName);

    /**
     * FUSE @c link callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    FileAttrPtr symlink(const folly::fbstring &parentUuid,
        const folly::fbstring &name, const folly::fbstring &link);

    /**
     * FUSE @c link callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    folly::fbstring readlink(const folly::fbstring &uuid);

    /**
     * FUSE @c unlink callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    void unlink(const folly::fbstring &parentUuid, const folly::fbstring &name);

    /**
     * FUSE @c rename callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    void rename(const folly::fbstring &parentUuid, const folly::fbstring &name,
        const folly::fbstring &newParentUuid, const folly::fbstring &newName);

    /**
     * FUSE @c setattr callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    FileAttrPtr setattr(
        const folly::fbstring &uuid, const struct stat &attr, const int toSet);

    /**
     * FUSE @c create callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    std::pair<FileAttrPtr, std::uint64_t> create(
        const folly::fbstring &parentUuid, const folly::fbstring &name,
        const mode_t mode, const int flags);

    /**
     * FUSE @c flush callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    void flush(const folly::fbstring &uuid, const std::uint64_t fileHandleId);

    /**
     * FUSE @c fsync callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    void fsync(const folly::fbstring &uuid, const std::uint64_t fileHandleId,
        const bool dataOnly);

    /**
     * FUSE @c getxattr callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    folly::fbstring getxattr(
        const folly::fbstring &uuid, const folly::fbstring &name);

    /**
     * FUSE @c setxattr callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    void setxattr(const folly::fbstring &uuid, const folly::fbstring &name,
        const folly::fbstring &value, bool create, bool replace);

    /**
     * FUSE @c removexattr callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    void removexattr(const folly::fbstring &uuid, const folly::fbstring &name);

    /**
     * FUSE @c listxattr callback.
     * @see https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html
     */
    folly::fbvector<folly::fbstring> listxattr(const folly::fbstring &uuid);

    /**
     * Sets a callback to be called when a file is marked as deleted.
     * @param cb The callback function that takes file's uuid as parameter.
     */
    void onMarkDeleted(std::function<void(const folly::fbstring &)> cb)
    {
        m_onMarkDeleted = std::move(cb);
    }

    /**
     * Sets a callback to be called when a file is marked as deleted.
     * @param cb The callback function that takes file's old uuid and new uuid
     * as parameters.
     */
    void onRename(std::function<void(const folly::fbstring &,
            const folly::fbstring &, const folly::fbstring &)>
            cb)
    {
        m_onRename = std::move(cb);
    }

    /**
     * Returns true if full block reads are forced.
     */
    bool isFullBlockReadForced() const { return m_forceFullblockRead; }

    std::shared_ptr<IOTraceLogger> ioTraceLogger() { return m_ioTraceLogger; }

    std::shared_ptr<FuseFileHandle> getFuseFileHandle(std::uint64_t handleId)
    {
        return m_fuseFileHandles.at(handleId);
    }

    std::map<folly::fbstring, folly::fbvector<std::pair<off_t, off_t>>>
    getFileLocalBlocks(const folly::fbstring &uuid);

    cache::OpenFileMetadataCache &metadataCache() { return m_metadataCache; }

    void setMaxRetryCount(int retryCount)
    {
        m_maxRetryCount = std::min(retryCount, FsLogic::MAX_RETRY_COUNT);
    }

    folly::fbstring rootUuid() const { return m_rootUuid; }

private:
    folly::IOBufQueue readInternal(const folly::fbstring &uuid,
        const std::uint64_t fileHandleId, const off_t offset,
        const std::size_t size, folly::Optional<folly::fbstring> checksum,
        const int retriesLeft = FSLOGIC_RETRY_COUNT,
        std::unique_ptr<IOTraceRead> ioTraceEntry = {});

    template <typename SrvMsg = messages::fuse::FuseResponse, typename CliMsg>
    SrvMsg communicate(CliMsg &&msg, const std::chrono::seconds timeout);

    folly::fbstring syncAndFetchChecksum(const folly::fbstring &uuid,
        const boost::icl::discrete_interval<off_t> &range);

    void sync(const folly::fbstring &uuid,
        const boost::icl::discrete_interval<off_t> &range);

    bool dataCorrupted(const folly::fbstring &uuid,
        const folly::IOBufQueue &buf, const folly::fbstring &serverChecksum,
        const boost::icl::discrete_interval<off_t> &availableRange,
        const boost::icl::discrete_interval<off_t> &wantedRange);

    folly::fbstring computeHash(const folly::IOBufQueue &buf);

    static folly::fbstring getFileIdFromFilename(const folly::fbstring &name);

    FileAttrPtr makeFile(const folly::fbstring &parentUuid,
        const folly::fbstring &name, const mode_t mode,
        const helpers::Flag flag);

    bool isSpaceDisabled(const folly::fbstring &spaceId);
    void disableSpaces(const std::vector<std::string> &spaces);

    std::shared_ptr<IOTraceLogger> createIOTraceLogger();

    std::pair<size_t, IOTraceLogger::PrefetchType> prefetchAsync(
        std::shared_ptr<FuseFileHandle> fuseFileHandle,
        const helpers::FileHandlePtr &helperHandle, const off_t offset,
        const std::size_t size, const folly::fbstring &uuid,
        const boost::icl::discrete_interval<off_t> possibleRange,
        const boost::icl::discrete_interval<off_t> availableRange);

    /**
     * Suspends current fiber for a random timed delay depending
     * on current retry number.
     *
     * @param retriesLeft Current number of retries left
     */
    void fiberRetryDelay(int retriesLeft);

    /**
     * Removes contents of expired directories from cache.
     *
     * @param delay Time in seconds after which contents of inactive
     * directories are purged from the metadata cache.
     */
    void pruneExpiredDirectories(const std::chrono::seconds delay);

    /**
     * Creates a space-relative path from a absolute path pointing to
     * an active oneclient mountpoint in the format:
     *   <__onedata_space_id:SPACE_ID>/dir1/dir2/file.txt
     *
     *  @param link The original absolute link passed to FsLogic
     *  @returns Space-relative link or original link if conversion fails
     */
    folly::fbstring createSpaceRelativeSymlink(const folly::fbstring &link);

    /**
     * Resolve a space-relative path to an absolute path starting with the
     * current oneclient mountpoint.
     *
     * @param link Space-relative link
     * @returns Oneclient mountpoint absolute path
     */
    folly::fbstring resolveSpaceRelativeSymlink(const folly::fbstring &link);

    std::shared_ptr<Context> m_context;
    events::Manager m_eventManager{m_context};
    cache::OpenFileMetadataCache m_metadataCache;
    cache::ForceProxyIOCache m_forceProxyIOCache;
    std::unique_ptr<cache::HelpersCache> m_helpersCache;
    std::shared_ptr<virtualfs::VirtualFsHelpersCache> m_virtualFsHelpersCache;
    std::shared_ptr<cache::ReaddirCache> m_readdirCache;
    bool m_readEventsDisabled = false;

    // Determines whether the read requests should return full requested
    // size, or can return partial byte range if it is immediately
    // available
    bool m_forceFullblockRead;
    FsSubscriptions m_fsSubscriptions;
    std::unordered_set<folly::fbstring> m_disabledSpaces;

    std::unordered_map<std::uint64_t, std::shared_ptr<FuseFileHandle>>
        m_fuseFileHandles;
    std::multimap<folly::fbstring, std::uint64_t> m_openFileHandles;
    std::unordered_map<std::uint64_t, int> m_fuseFileHandleFlags;
    std::unordered_map<std::uint64_t, folly::fbstring> m_fuseDirectoryHandles;
    std::atomic<std::uint64_t> m_nextFuseHandleId{1};

    std::function<void(const folly::fbstring &)> m_onMarkDeleted = [](auto) {};
    std::function<void(const folly::fbstring &, const folly::fbstring &,
        const folly::fbstring &)>
        m_onRename = [](auto, auto, auto) {};

    const std::chrono::seconds m_providerTimeout;
    const std::chrono::seconds m_storageTimeout;
    std::function<void(folly::Function<void()>)> m_runInFiber;

    const bool m_prefetchModeAsync;
    const unsigned int m_minPrefetchBlockSize;
    const double m_linearReadPrefetchThreshold;
    const double m_randomReadPrefetchThreshold;
    const unsigned int m_randomReadPrefetchBlockThreshold;
    const int m_randomReadPrefetchClusterWindow;
    const unsigned int m_randomReadPrefetchClusterBlockThreshold;
    const unsigned int m_randomReadPrefetchEvaluationFrequency;
    const double m_randomReadPrefetchClusterWindowGrowFactor;
    const bool m_clusterPrefetchThresholdRandom;
    const bool m_showOnlyFullReplicas;
    const bool m_showSpaceIdsNotNames;
    const bool m_showHardLinkCount;
    const bool m_ioTraceLoggerEnabled;
    const boost::optional<std::pair<std::string, std::string>> m_tagOnCreate;
    const boost::optional<std::pair<std::string, std::string>> m_tagOnModify;
    const folly::fbstring m_rootUuid;

    std::shared_ptr<IOTraceLogger> m_ioTraceLogger;

    std::random_device m_clusterPrefetchRD{};
    std::mt19937 m_clusterPrefetchRandomGenerator{m_clusterPrefetchRD()};
    std::uniform_int_distribution<> m_clusterPrefetchDistribution;

    folly::fibers::Baton m_directoryCachePruneBaton;
    std::atomic_bool m_stopped = ATOMIC_VAR_INIT(false);
    int m_maxRetryCount{FsLogic::MAX_RETRY_COUNT};
};
} // namespace fslogic
} // namespace client
} // namespace one
