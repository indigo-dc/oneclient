/**
 * @file fsLogic.cc
 * @author Konrad Zemek
 * @copyright (C) 2016 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#include "fsLogic.h"

#include "communication/communicator.h"
#include "context.h"
#include "fslogic/virtualfs/archivematica.h"
#include "helpers/logging.h"
#include "messages/closeSession.h"
#include "messages/configuration.h"
#include "messages/fuse/blockSynchronizationRequest.h"
#include "messages/fuse/changeMode.h"
#include "messages/fuse/createDir.h"
#include "messages/fuse/createFile.h"
#include "messages/fuse/deleteFile.h"
#include "messages/fuse/fileAttr.h"
#include "messages/fuse/fileBlock.h"
#include "messages/fuse/fileChildren.h"
#include "messages/fuse/fileChildrenAttrs.h"
#include "messages/fuse/fileCreated.h"
#include "messages/fuse/fileLocation.h"
#include "messages/fuse/fileLocationChanged.h"
#include "messages/fuse/fileOpened.h"
#include "messages/fuse/fileRenamed.h"
#include "messages/fuse/fileRenamedEntry.h"
#include "messages/fuse/fsStats.h"
#include "messages/fuse/fsync.h"
#include "messages/fuse/getFSStats.h"
#include "messages/fuse/getFileChildren.h"
#include "messages/fuse/getFileChildrenAttrs.h"
#include "messages/fuse/getHelperParams.h"
#include "messages/fuse/getXAttr.h"
#include "messages/fuse/helperParams.h"
#include "messages/fuse/listXAttr.h"
#include "messages/fuse/makeFile.h"
#include "messages/fuse/makeLink.h"
#include "messages/fuse/makeSymLink.h"
#include "messages/fuse/openFile.h"
#include "messages/fuse/readSymLink.h"
#include "messages/fuse/release.h"
#include "messages/fuse/removeXAttr.h"
#include "messages/fuse/rename.h"
#include "messages/fuse/setXAttr.h"
#include "messages/fuse/symLink.h"
#include "messages/fuse/syncResponse.h"
#include "messages/fuse/synchronizeBlock.h"
#include "messages/fuse/synchronizeBlockAndComputeChecksum.h"
#include "messages/fuse/truncate.h"
#include "messages/fuse/updateTimes.h"
#include "messages/fuse/xattr.h"
#include "messages/fuse/xattrList.h"
#include "monitoring/monitoring.h"
#include "util/cdmi.h"
#include "util/uuid.h"
#include "util/xattrHelper.h"

#include <boost/icl/interval_set.hpp>
#include <folly/Demangle.h>
#include <folly/Range.h>
#include <folly/ScopeGuard.h>
#include <folly/container/Enumerate.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManager.h>
#include <folly/fibers/ForEach.h>
#include <folly/json.h>
#if FUSE_USE_VERSION > 30
#include <fuse3/fuse_lowlevel.h>
#else
#include <fuse/fuse_lowlevel.h>
#endif

#include <openssl/md4.h>

#include "buffering/bufferAgent.h"

#define IOTRACE_START() auto __ioTraceStart = std::chrono::system_clock::now();

#define IOTRACE_END(TraceType, optype, uuid, handleId, ...)                    \
    if (m_ioTraceLoggerEnabled)                                                \
        m_ioTraceLogger->log(TraceType(__ioTraceStart, optype,                 \
            std::chrono::duration_cast<std::chrono::microseconds>(             \
                std::chrono::system_clock::now() - __ioTraceStart),            \
            uuid, handleId, 0, ##__VA_ARGS__));

#define IOTRACE_GUARD(TraceType, optype, uuid, handleId, ...)                  \
    IOTRACE_START()                                                            \
    auto __ioTraceGuard = folly::makeGuard([&] {                               \
        IOTRACE_END(TraceType, optype, uuid, handleId, ##__VA_ARGS__)          \
    });

namespace one {
namespace client {
namespace fslogic {

using namespace std::literals;

namespace {
const std::string kAbsLinkPrefix = "<__onedata_space_id:"; // NOLINT
} // namespace

/**
 * Filters given flags set to one of RDONLY, WRONLY or RDWR.
 * Returns RDONLY if flag value is zero.
 * @param Flags value
 */
inline helpers::Flag getOpenFlag(const helpers::FlagsSet &flagsSet)
{
    if (flagsSet.count(one::helpers::Flag::RDONLY) > 0)
        return one::helpers::Flag::RDONLY;
    if (flagsSet.count(one::helpers::Flag::WRONLY) > 0)
        return one::helpers::Flag::WRONLY;
    if (flagsSet.count(one::helpers::Flag::RDWR) > 0)
        return one::helpers::Flag::RDWR;

    return one::helpers::Flag::RDONLY;
}

constexpr auto XATTR_FILE_BLOCKS_MAP_LENGTH = 50;
constexpr auto LINEAR_PREFETCH_THRESHOLD_MATCH_RATIO = 0.9;

inline static folly::fbstring ONE_XATTR(const std::string &name)
{
    assert(!name.empty());
    return ONE_XATTR_PREFIX + name;
}

FsLogic::FsLogic(std::shared_ptr<Context> context,
    std::shared_ptr<messages::Configuration> configuration,
    std::unique_ptr<cache::HelpersCache> helpersCache,
    unsigned int metadataCacheSize, bool readEventsDisabled,
    bool forceFullblockRead, const std::chrono::seconds providerTimeout,
    const std::chrono::seconds directoryCacheDropAfter,
    std::function<void(folly::Function<void()>)> runInFiber)
    : m_context{context}
    , m_metadataCache{*m_context->communicator(), metadataCacheSize,
          providerTimeout, directoryCacheDropAfter, configuration->rootUuid(),
          m_context->options()->getSpaceNames(),
          m_context->options()->getSpaceIds(),
          m_context->options()->showOnlyFullReplicas(),
          m_context->options()->showHardLinkCount(),
          m_context->options()->showSpaceIds()}
    , m_helpersCache{std::move(helpersCache)}
    , m_virtualFsHelpersCache{std::make_shared<
          virtualfs::VirtualFsHelpersCache>(*this)}
    , m_readdirCache{std::make_shared<cache::ReaddirCache>(
          m_metadataCache, m_context, m_virtualFsHelpersCache, runInFiber)}
    , m_readEventsDisabled{readEventsDisabled}
    , m_forceFullblockRead{forceFullblockRead}
    , m_fsSubscriptions{m_eventManager, m_metadataCache, m_forceProxyIOCache,
          runInFiber}
    , m_nextFuseHandleId{0}
    , m_providerTimeout{providerTimeout}
    , m_storageTimeout{m_context->options()->getStorageTimeout()}
    , m_runInFiber{std::move(runInFiber)} /* clang-format off */
    , m_prefetchModeAsync{m_context->options()->getPrefetchMode() == "async"}
    , m_minPrefetchBlockSize{m_context->options()->getMinimumBlockPrefetchSize()}
    , m_linearReadPrefetchThreshold{m_context->options()
          ->getLinearReadPrefetchThreshold()}
    , m_randomReadPrefetchThreshold{m_context->options()
          ->getRandomReadPrefetchThreshold()}
    , m_randomReadPrefetchBlockThreshold{m_context->options()
          ->getRandomReadPrefetchBlockThreshold()}
    , m_randomReadPrefetchClusterWindow{m_context->options()
          ->getRandomReadPrefetchClusterWindow()}
    , m_randomReadPrefetchClusterBlockThreshold{m_context->options()
          ->getRandomReadPrefetchClusterBlockThreshold()}
    , m_randomReadPrefetchEvaluationFrequency{m_context->options()
          ->getRandomReadPrefetchEvaluationFrequency()}
    , m_randomReadPrefetchClusterWindowGrowFactor{m_context->options()
          ->getRandomReadPrefetchClusterWindowGrowFactor()}
    , m_clusterPrefetchThresholdRandom{m_context->options()
          ->isClusterPrefetchThresholdRandom()}
    , m_showOnlyFullReplicas{m_context->options()->showOnlyFullReplicas()}
    , m_showSpaceIdsNotNames{m_context->options()->showSpaceIds()}
    , m_showHardLinkCount{m_context->options()->showHardLinkCount()}
    , m_ioTraceLoggerEnabled{m_context->options()->isIOTraceLoggerEnabled()}
    , m_tagOnCreate{m_context->options()->getOnCreateTag()}
    , m_tagOnModify{m_context->options()->getOnModifyTag()}
    , m_rootUuid{configuration->rootUuid()}
/* clang-format on */
{
    m_nextFuseHandleId = 0;

    m_runInFiber([this]() {
        auto tid = std::this_thread::get_id();
        setFiberThreadId(tid);
        m_metadataCache.setFiberThreadId(tid);
        m_readdirCache->setFiberThreadId(tid);
    });

    m_eventManager.subscribe(*configuration);

    m_metadataCache.setReaddirCache(m_readdirCache);

    m_metadataCache.setVirtualFsHelpersCache(m_virtualFsHelpersCache);

    // Register Archivematica virtual fs
    if (m_context->options()->isArchivematicaModeEnabled())
        m_virtualFsHelpersCache->add("archivematica",
            std::make_shared<
                virtualfs::archivematica::ArchivematicaVirtualFsAdapter>(
                *this, "archivematica"));

    disableSpaces(configuration->disabledSpaces());

    //
    // Registration of force proxy IO cache callbacks
    //
    m_forceProxyIOCache.onAdd([this](const folly::fbstring &uuid) {
        m_fsSubscriptions.subscribeFilePermChanged(uuid);
    });

    m_forceProxyIOCache.onRemove([this](const folly::fbstring &uuid) {
        m_fsSubscriptions.unsubscribeFilePermChanged(uuid);
    });

    //
    // Registration of medatacache events callbacks
    //
    // Called when file attributes are added to the metadata cache
    m_metadataCache.onAdd([this](const folly::fbstring &uuid) {
        m_fsSubscriptions.subscribeFileAttrChanged(uuid);
        m_fsSubscriptions.subscribeFileRemoved(uuid);
        m_fsSubscriptions.subscribeFileRenamed(uuid);

        if (m_showOnlyFullReplicas)
            m_fsSubscriptions.subscribeReplicaStatusChanged(uuid);
    });

    // Called when file is opened
    m_metadataCache.onOpen([this](const folly::fbstring &uuid) {
        m_fsSubscriptions.subscribeFileAttrChanged(uuid);
        m_fsSubscriptions.subscribeFileLocationChanged(uuid);
        m_fsSubscriptions.subscribeFileRemoved(uuid);
        m_fsSubscriptions.subscribeFileRenamed(uuid);
    });

    // Called when file is closed
    m_metadataCache.onRelease([this](const folly::fbstring &uuid) {
        m_fsSubscriptions.unsubscribeFileLocationChanged(uuid);
    });

    // Called when file attributes are dropped from metadata cache
    m_metadataCache.onDropFile([this](const folly::fbstring &uuid) {
        m_fsSubscriptions.unsubscribeFileAttrChanged(uuid);
        m_fsSubscriptions.unsubscribeFileLocationChanged(uuid);
        m_fsSubscriptions.unsubscribeFileRemoved(uuid);
        m_fsSubscriptions.unsubscribeFileRenamed(uuid);
        if (m_showOnlyFullReplicas)
            m_fsSubscriptions.unsubscribeReplicaStatusChanged(uuid);
    });

    // Called when directory attributes are dropped from metadata cache
    m_metadataCache.onDropDirectory([this](const folly::fbstring &uuid) {
        m_fsSubscriptions.unsubscribeFileAttrChanged(uuid);
        m_fsSubscriptions.unsubscribeFileRemoved(uuid);
        m_fsSubscriptions.unsubscribeFileRenamed(uuid);
        if (m_showOnlyFullReplicas)
            m_fsSubscriptions.unsubscribeReplicaStatusChanged(uuid);
    });

    // Called when file is renamed
    m_metadataCache.onRename(
        [this](const folly::fbstring &oldUuid, const folly::fbstring &newUuid) {
            if (oldUuid != newUuid) {
                m_fsSubscriptions.unsubscribeFileAttrChanged(oldUuid);
                m_fsSubscriptions.unsubscribeFileRemoved(oldUuid);
                m_fsSubscriptions.unsubscribeFileRenamed(oldUuid);
                if (m_showOnlyFullReplicas)
                    m_fsSubscriptions.unsubscribeReplicaStatusChanged(oldUuid);
                m_fsSubscriptions.subscribeFileAttrChanged(newUuid);
                m_fsSubscriptions.subscribeFileRemoved(newUuid);
                m_fsSubscriptions.subscribeFileRenamed(newUuid);
                if (m_showOnlyFullReplicas)
                    m_fsSubscriptions.subscribeReplicaStatusChanged(newUuid);

                if (m_fsSubscriptions.unsubscribeFileLocationChanged(oldUuid))
                    m_fsSubscriptions.subscribeFileLocationChanged(newUuid);
            }
            m_onRename(oldUuid, newUuid);
        });

    // Called when file is removed
    m_metadataCache.onMarkDeleted(
        [this](const folly::fbstring &uuid) { m_onMarkDeleted(uuid); });

    if (m_clusterPrefetchThresholdRandom) {
        m_clusterPrefetchDistribution = std::uniform_int_distribution<int>(
            2, m_randomReadPrefetchClusterBlockThreshold);
    }

    if (m_ioTraceLoggerEnabled) {
        m_ioTraceLogger = createIOTraceLogger();
        IOTRACE_GUARD(IOTraceMount, IOTraceLogger::OpType::MOUNT,
            configuration->rootUuid(), 0,
            context->options()->getMountpoint().string());
    }

    m_runInFiber([this, directoryCacheDropAfter]() {
        pruneExpiredDirectories(directoryCacheDropAfter);
    });

    m_context->communicator()->setOnConnectionLostCallback([this]() {
        LOG_DBG(2) << "Called on connection lost callback";
        m_runInFiber([this]() { reset(); });
    });

    m_context->communicator()->setOnReconnectCallback([this]() {
        LOG_DBG(2) << "Called on reconnect callback";
        m_runInFiber([]() {});
    });

    start();
}

FsLogic::~FsLogic() { stop(); }

void FsLogic::start()
{
    // Quota initial configuration
    m_eventManager.subscribe(
        events::QuotaExceededSubscription{[=](auto events) {
            m_runInFiber([this, events = std::move(events)] {
                this->disableSpaces(events.back()->spaces());
            });
        }});

    m_stopped = false;
}

void FsLogic::stop()
{
    if (!m_stopped) {
        m_stopped = true;

        m_fsSubscriptions.unsubscribeAll();
        m_fsSubscriptions.stop();

        m_directoryCachePruneBaton.post();

        m_context->communicator()->send(messages::CloseSession{}, 1).get();

        m_context->communicator()->stop();
    }
}

void FsLogic::reset()
{
    LOG_DBG(1) << "Resetting internal caches after connection lost...";

    // Close all files
    for (auto &fh : m_fuseFileHandles) {
        fh.second->reset();
    }
    m_fuseFileHandles.clear();
    m_openFileHandles.clear();

    // Clear metadata cache
    m_metadataCache.clear();

    // Cancel all subscriptions
    m_fsSubscriptions.unsubscribeAll();

    m_eventManager.reset();
}

constexpr int FsLogic::MAX_RETRY_COUNT; // NOLINT

struct statvfs FsLogic::statfs(const folly::fbstring &uuid)
{
    LOG_FCALL() << LOG_FARG(uuid);

    assertInFiber();

    constexpr auto kMaxNameLength = 255;
    constexpr auto kBlockSize = 4096;
    constexpr auto kFreeInodes = 10000000;

    auto emulatedFreeSpace = m_context->options()->getEmulateAvailableSpace();

    struct statvfs statinfo = {};

    if (emulatedFreeSpace > 0ULL) {
        statinfo.f_bsize = kBlockSize;
        statinfo.f_frsize = statinfo.f_bsize;
        statinfo.f_blocks = statinfo.f_bfree = statinfo.f_bavail =
            emulatedFreeSpace / statinfo.f_frsize;
        statinfo.f_files = statinfo.f_ffree = kFreeInodes;
    }
    else {
        size_t totalSize = 0;
        size_t totalFreeSize = 0;
        if (uuid != m_rootUuid) {
            // Handle statfs for a specific space or a file inside a space
            messages::fuse::GetFSStats msg{uuid.toStdString()};
            auto fsStats = communicate<messages::fuse::FSStats>(
                std::move(msg), m_providerTimeout);
            totalSize = fsStats.getTotalSize();
            totalFreeSize = fsStats.getTotalFreeSize();
        }
        else {
            // Handle statfs for entire mountpoint by summing storage size
            // on all accessible spaces
            constexpr auto kMaxStatFSSpaceCount = 1024;
            auto spaces = readdir(uuid, kMaxStatFSSpaceCount, 0);
            for (const auto &space : spaces) {
                if (space == "." || space == "..")
                    continue;

                try {
                    auto spaceAttrs = lookup(uuid, space);

                    messages::fuse::GetFSStats msg{
                        spaceAttrs->uuid().toStdString()};
                    auto fsStats = communicate<messages::fuse::FSStats>(
                        std::move(msg), m_providerTimeout);

                    totalSize += fsStats.getTotalSize();
                    totalFreeSize += fsStats.getTotalFreeSize();
                }
                catch (const std::system_error &e) {
                    if (e.code().value() == ENOENT)
                        continue;

                    throw e;
                }
            }
        }

        // block and fragment size
        statinfo.f_frsize = statinfo.f_bsize = kBlockSize;
        // size of fs in f_frsize units
        statinfo.f_blocks = std::ceil(static_cast<double>(totalSize) /
            static_cast<double>(statinfo.f_frsize));
        // free blocks for privileged and unprivileged users
        statinfo.f_bfree = statinfo.f_bavail =
            std::ceil(static_cast<double>(totalFreeSize) /
                static_cast<double>(statinfo.f_frsize));
        // free inodes for privileged and unprivileged users
        statinfo.f_ffree = statinfo.f_favail = kFreeInodes;
    }
    statinfo.f_namemax = kMaxNameLength;

    return statinfo;
}

FileAttrPtr FsLogic::lookup(
    const folly::fbstring &uuid, const folly::fbstring &name)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(name);

    IOTRACE_START()

    assertInFiber();

    auto fileNameUUID = getFileIdFromFilename(name);
    FileAttrPtr attr{};

    bool tryVirtualFile{false};
    std::exception_ptr current_exception;
    try {
        if (fileNameUUID.empty())
            attr = m_metadataCache.getAttr(uuid, name);
        else
            attr = m_metadataCache.getAttr(fileNameUUID);
    }
    catch (std::system_error &e) {
        if (e.code() == std::errc::no_such_file_or_directory) {
            tryVirtualFile = true;
            current_exception = std::current_exception();
        }
        else
            throw e;
    }

    if (tryVirtualFile) {
        if (m_metadataCache.getAttr(uuid)->isVirtual()) {
            // Force update of the directory contents and lookup
            // the file again
            readdir(uuid, std::numeric_limits<int>::max(), 0);
            attr = m_metadataCache.getAttr(uuid, name);
        }
        else
            std::rethrow_exception(current_exception);
    }

    if (!attr)
        throw std::system_error(
            std::make_error_code(std::errc::no_such_file_or_directory));

    const auto *type =
        attr->type() == FileAttr::FileType::directory ? "d" : "f";
    auto size = attr->size();

    IOTRACE_END(IOTraceLookup, IOTraceLogger::OpType::LOOKUP, uuid, 0, name,
        attr->uuid(), type, size ? *size : 0);

    return attr;
}

FileAttrPtr FsLogic::getattr(const folly::fbstring &uuid)
{
    LOG_FCALL() << LOG_FARG(uuid);

    IOTRACE_GUARD(IOTraceGetAttr, IOTraceLogger::OpType::GETATTR, uuid, 0)

    assertInFiber();

    auto attr = m_metadataCache.getAttr(uuid);

    if (attr->type() == FileAttr::FileType::symlink) {
        // If this is a symlink, return an attr with the size set to a
        // length of a resolved symlink
        auto symlinkAttr = std::make_shared<FileAttr>(*attr);
        symlinkAttr->size(readlink(uuid).size());
        return symlinkAttr;
    }

    return attr;
}

std::uint64_t FsLogic::opendir(const folly::fbstring &uuid)
{
    LOG_FCALL() << LOG_FARG(uuid);

    assertInFiber();

    // Check that directory exists
    auto attr = m_metadataCache.getAttr(uuid);

    if (attr->type() != FileAttr::FileType::directory)
        throw std::system_error(
            std::make_error_code(std::errc::no_such_file_or_directory));

    const auto fuseFileHandleId = m_nextFuseHandleId++;

    m_metadataCache.opendir(uuid);

    return fuseFileHandleId;
}

void FsLogic::releasedir(
    const folly::fbstring &uuid, const std::uint64_t /*fileHandleId*/)
{
    LOG_FCALL() << LOG_FARG(uuid);

    assertInFiber();

    m_metadataCache.releasedir(uuid);
}

folly::fbvector<folly::fbstring> FsLogic::readdir(
    const folly::fbstring &uuid, const size_t maxSize, const off_t off)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(maxSize) << LOG_FARG(off);

    IOTRACE_START()

    assertInFiber();

    auto entries = m_readdirCache->readdir(
        uuid, off, maxSize, m_showOnlyFullReplicas, m_showHardLinkCount);

    IOTRACE_END(IOTraceReadDir, IOTraceLogger::OpType::READDIR, uuid, 0,
        maxSize, off, entries.size())

    return entries;
}

std::uint64_t FsLogic::open(const folly::fbstring &uuid, const int flags,
    const std::uint64_t reuseFuseFileHandleId)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARGH(flags);

    IOTRACE_START()

    assertInFiber();

    auto openFileToken = m_metadataCache.open(uuid);

    const auto filteredFlags = flags & (~O_CREAT) & (~O_APPEND);
    const auto flag = getOpenFlag(helpers::maskToFlags(filteredFlags));
    messages::fuse::OpenFile msg{uuid.toStdString(), flag};

    // Check if the file is a virtual file
    auto attr = m_metadataCache.getAttr(uuid);

    auto fuseFileHandleId = reuseFuseFileHandleId;
    if (fuseFileHandleId == 0U)
        fuseFileHandleId = m_nextFuseHandleId++;

    if (attr->isVirtual()) {
        // Create a virtual file handle id
        // Register the file handle in fusefilehandles
        m_fuseFileHandles.emplace(fuseFileHandleId,
            std::make_shared<FuseFileHandle>(filteredFlags,
                std::to_string(fuseFileHandleId), openFileToken,
                *m_virtualFsHelpersCache, m_forceProxyIOCache,
                m_providerTimeout, m_randomReadPrefetchEvaluationFrequency));

        m_openFileHandles.emplace(uuid, fuseFileHandleId);
        auto fuseFileHandle = m_fuseFileHandles.at(fuseFileHandleId);
        fuseFileHandle->getHelperHandle(
            uuid, {}, attr->getVirtualFsAdapter()->name(), uuid);
        m_fuseFileHandleFlags.emplace(fuseFileHandleId, flags);

        // Return fuse file handle id
        return fuseFileHandleId;
    }

    LOG_DBG(2) << "Sending file opened message for " << uuid;

    auto opened = communicate<messages::fuse::FileOpened>(
        std::move(msg), m_providerTimeout);

    m_fuseFileHandles.emplace(fuseFileHandleId,
        std::make_shared<FuseFileHandle>(filteredFlags, opened.handleId(),
            openFileToken, *m_helpersCache, m_forceProxyIOCache,
            m_providerTimeout, m_randomReadPrefetchEvaluationFrequency));
    m_openFileHandles.emplace(uuid, fuseFileHandleId);
    m_fuseFileHandleFlags.emplace(fuseFileHandleId, flags);

    IOTRACE_END(
        IOTraceOpen, IOTraceLogger::OpType::OPEN, uuid, fuseFileHandleId, flags)

    LOG_DBG(2) << "Assigned fuse handle " << fuseFileHandleId << " for file "
               << uuid;

    return fuseFileHandleId;
}

void FsLogic::release(
    const folly::fbstring &uuid, const std::uint64_t fileHandleId)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(fileHandleId);

    IOTRACE_GUARD(
        IOTraceRelease, IOTraceLogger::OpType::RELEASE, uuid, fileHandleId)

    assertInFiber();

    if (m_fuseFileHandles.find(fileHandleId) == m_fuseFileHandles.cend()) {
        LOG_DBG(1) << "Fuse file handle " << fileHandleId
                   << " already released.";
        return;
    }

    auto attr = m_metadataCache.getAttr(uuid);
    auto isVirtualFile = attr->isVirtual();

    auto fuseFileHandle = m_fuseFileHandles.at(fileHandleId);

    fsync(uuid, fileHandleId, false);

    folly::fbvector<folly::Future<folly::Unit>> releaseFutures;
    for (auto &helperHandle : fuseFileHandle->helperHandles())
        releaseFutures.emplace_back(helperHandle->release());

    auto releaseExceptionFuture =
        folly::collectAll(releaseFutures)
            .via(folly::getCPUExecutor().get())
            .thenValue([](std::vector<folly::Try<folly::Unit>> &&tries) {
                for (auto &t : tries)
                    t.value();
            });

    std::exception_ptr releaseException;
    try {
        LOG_DBG(2) << "Releasing local file handles for " << uuid;

        communication::wait(
            std::move(releaseExceptionFuture), m_providerTimeout);

        LOG_DBG(2) << "Sending file release message for " << uuid;

        // If the file is not a virtual file, send release message to
        // the Oneprovider
        if (!isVirtualFile)
            communicate(messages::fuse::Release{uuid.toStdString(),
                            fuseFileHandle->providerHandleId()->toStdString()},
                m_providerTimeout);
    }
    catch (const std::system_error &e) {
        if (e.code().value() == ENOENT)
            LOG_DBG(1) << "File release request ignore as the file " << uuid
                       << " is already deleted";
        else
            releaseException = std::current_exception();
    }
    catch (const std::exception &e) {
        LOG(WARNING) << "File release failed: " << e.what();
        releaseException = std::current_exception();
    }
    catch (...) {
        LOG(WARNING) << "File release failed: unknown error";
        releaseException = std::current_exception();
    }

    m_fuseFileHandles.erase(fileHandleId);
    auto pairIt = m_openFileHandles.equal_range(uuid);
    auto it = pairIt.first;
    for (; it != pairIt.second; ++it) {
        if (it->second == fileHandleId) {
            m_openFileHandles.erase(it);
            break;
        }
    }
    m_fuseFileHandleFlags.erase(fileHandleId);

    if (releaseException)
        std::rethrow_exception(releaseException);
}

void FsLogic::flush(
    const folly::fbstring &uuid, const std::uint64_t fileHandleId)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(fileHandleId);

    IOTRACE_GUARD(
        IOTraceFlush, IOTraceLogger::OpType::FLUSH, uuid, fileHandleId)

    assertInFiber();

    if (m_fuseFileHandles.find(fileHandleId) == m_fuseFileHandles.cend()) {
        LOG_DBG(1) << "Fuse file handle " << fileHandleId
                   << " already released.";
        return;
    }

    auto fuseFileHandle = m_fuseFileHandles.at(fileHandleId);

    LOG_DBG(2) << "Sending file flush message for " << uuid;

    for (auto &helperHandle : fuseFileHandle->helperHandles())
        communication::wait(helperHandle->flush(), m_storageTimeout);
}

void FsLogic::fsync(const folly::fbstring &uuid,
    const std::uint64_t fileHandleId, const bool dataOnly)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(fileHandleId)
                << LOG_FARG(dataOnly);

    IOTRACE_GUARD(IOTraceFsync, IOTraceLogger::OpType::FSYNC, uuid,
        fileHandleId, dataOnly)

    assertInFiber();

    if (m_fuseFileHandles.find(fileHandleId) == m_fuseFileHandles.cend()) {
        LOG_DBG(1) << "Fuse file handle " << fileHandleId
                   << " already released.";
        return;
    }

    m_eventManager.flush();

    auto fuseFileHandle = m_fuseFileHandles.at(fileHandleId);

    LOG_DBG(2) << "Sending file fsync message for " << uuid;

    auto attr = m_metadataCache.getAttr(uuid);
    auto isVirtualFile = attr->isVirtual();
    if (!isVirtualFile)
        communicate(messages::fuse::FSync{uuid.toStdString(), dataOnly,
                        fuseFileHandle->providerHandleId()->toStdString()},
            m_providerTimeout);

    for (auto &helperHandle : fuseFileHandle->helperHandles())
        communication::wait(helperHandle->fsync(dataOnly), m_storageTimeout);
}

folly::IOBufQueue FsLogic::read(const folly::fbstring &uuid,
    const std::uint64_t fileHandleId, const off_t offset,
    const std::size_t size, folly::Optional<folly::fbstring> checksum,
    const int retriesLeft, std::unique_ptr<IOTraceRead> ioTraceEntry)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(fileHandleId) << LOG_FARG(offset)
                << LOG_FARG(size);

    assertInFiber();

    if (m_ioTraceLoggerEnabled && !ioTraceEntry) {
        ioTraceEntry = std::make_unique<IOTraceRead>();
        ioTraceEntry->opType = IOTraceLogger::OpType::READ;
        ioTraceEntry->uuid = uuid;
        ioTraceEntry->handleId = fileHandleId;
        ioTraceEntry->retries = 0;
        std::get<0>(ioTraceEntry->arguments) = offset;
        std::get<1>(ioTraceEntry->arguments) = size;
        std::get<2>(ioTraceEntry->arguments) = true;
        std::get<3>(ioTraceEntry->arguments) = 0;
        std::get<4>(ioTraceEntry->arguments) =
            IOTraceLogger::toString(IOTraceLogger::PrefetchType::NONE);
    }

    if (!m_context->communicator()->isConnected()) {
        LOG(ERROR) << "Connection to Oneprovider lost...";
        if (retriesLeft > 0) {
            fiberRetryDelay(retriesLeft);
            return read(uuid, fileHandleId, offset, size, checksum,
                retriesLeft - 1, std::move(ioTraceEntry));
        }

        throw std::system_error(std::make_error_code(std::errc::timed_out));
    }

    if (m_fuseFileHandles.find(fileHandleId) == m_fuseFileHandles.end()) {
        // File was probably opened during a network failure, reopen it
        // Get open flags from previous open
        auto flagIt = m_fuseFileHandleFlags.find(fileHandleId);
        if (flagIt == m_fuseFileHandleFlags.end()) {
            LOG(ERROR) << "Cannot reopen file " << uuid
                       << " - no cached open flags...";
            throw std::system_error(
                std::make_error_code(std::errc::bad_file_descriptor));
        }
        LOG(INFO) << "Reopening file " << uuid
                  << " after network connection failure...";
        m_metadataCache.getAttr(uuid);
        open(uuid, flagIt->second, fileHandleId);
    }

    auto fuseFileHandle = m_fuseFileHandles.at(fileHandleId);
    auto attr = m_metadataCache.getAttr(uuid);

    const auto fileSize = *attr->size();
    const auto possibleRange =
        boost::icl::discrete_interval<off_t>::right_open(0, fileSize);

    const auto requestedRange =
        boost::icl::discrete_interval<off_t>::right_open(offset, offset + size);

    auto wantedRange = requestedRange & possibleRange;

    if (boost::icl::size(wantedRange) <= 0) {
        LOG_DBG(2) << "Read requested for impossible range " << requestedRange
                   << " for file " << uuid;
        return folly::IOBufQueue{folly::IOBufQueue::cacheChainLength()};
    }

    LOG_DBG(2) << "FsLogic reading from file " << uuid << " in range "
               << wantedRange;

    // Even if several "touching" blocks with different helpers are
    // available to read right now, for simplicity we'll only read a single
    // block per a read operation.
    int ec = 0;
    folly::exception_wrapper ew;
    try {
        auto locationData = m_metadataCache.getBlock(uuid, offset);
        if (!locationData.hasValue()) {
            LOG_DBG(2) << "Requested block for " << uuid
                       << " not yet replicated - fetching from remote provider";

            auto defaultBlock = m_metadataCache.getDefaultBlock(uuid);
            auto helperHandle = fuseFileHandle->getHelperHandle(uuid,
                m_metadataCache.getSpaceId(uuid), defaultBlock.storageId(),
                defaultBlock.fileId());

            // In order to optimize the on the fly transfer between
            // Oneproviders, always request sync in at least the size as
            // specified by minPrefetchBlockSize on the command line
            // This minimizes the number of transfer requests for applications
            // which read in single kilobytes
            auto syncPrefetchRange =
                boost::icl::discrete_interval<off_t>::right_open(offset,
                    offset +
                        std::max<std::size_t>(size, m_minPrefetchBlockSize));

            sync(uuid, syncPrefetchRange & possibleRange);

            // Some storage helpers may require additional data consistency
            // check after remote transfer, to ensure that the data has been
            // flushed to storage
            folly::Optional<folly::fbstring> csum;
            if (helperHandle->needsDataConsistencyCheck()) {
                csum = syncAndFetchChecksum(uuid, wantedRange);
            }

            if (m_ioTraceLoggerEnabled)
                std::get<2>(ioTraceEntry->arguments) = false;

            if (retriesLeft >= 0) {
                return read(uuid, fileHandleId, offset, size, std::move(csum),
                    retriesLeft - 1, std::move(ioTraceEntry));
            }

            LOG(INFO) << "Cannot synchronize block " << wantedRange << " after "
                      << m_maxRetryCount << " retries "
                      << " in file " << uuid << " - returning block of zeros";

            auto iobuf = folly::IOBuf::create(size);
            memset(iobuf->writableTail(), 0, size);

            folly::IOBufQueue zeros{folly::IOBufQueue::cacheChainLength()};
            zeros.append(std::move(iobuf));
            return zeros;
        }

        boost::icl::discrete_interval<off_t> availableRange;
        messages::fuse::FileBlock fileBlock;
        std::tie(availableRange, fileBlock) = std::move(*locationData);
        const auto wantedAvailableRange = availableRange & wantedRange;

        LOG_DBG(2) << "Available block range for file " << uuid
                   << " in requested range: " << wantedAvailableRange;

        const std::size_t availableSize =
            boost::icl::size(wantedAvailableRange);

        auto helperHandle = fuseFileHandle->getHelperHandle(uuid,
            m_metadataCache.getSpaceId(uuid), fileBlock.storageId(),
            fileBlock.fileId());

        if (checksum) {
            LOG_DBG(1) << "Waiting on helper flush for " << uuid
                       << " due to required checksum";
            communication::wait(
                helperHandle->flushUnderlying(), m_storageTimeout);
        }

        auto prefetchParams = prefetchAsync(fuseFileHandle, helperHandle,
            offset, availableSize, uuid, possibleRange, availableRange);

        if (m_ioTraceLoggerEnabled) {
            std::get<3>(ioTraceEntry->arguments) = prefetchParams.first;
            std::get<4>(ioTraceEntry->arguments) =
                IOTraceLogger::toString(prefetchParams.second);
        }

        const std::size_t continuousSize =
            boost::icl::size(boost::icl::left_subtract(availableRange,
                boost::icl::discrete_interval<off_t>::right_open(0, offset)));

        LOG_DBG(2) << "Reading " << availableSize << " bytes from " << uuid
                   << " at offset " << offset;

        using one::logging::log_timer;
        using one::logging::csv::log;
        using one::logging::csv::read_write_perf;

        log_timer<> timer;

        auto readBuffer = communication::wait(
            helperHandle->readContinuous(offset, availableSize, continuousSize),
            m_storageTimeout);

        log<read_write_perf>(
            fileBlock.fileId(), "FsLogic", "read", offset, size, timer.stop());

        if (helperHandle->needsDataConsistencyCheck() && checksum &&
            dataCorrupted(uuid, readBuffer, *checksum, wantedAvailableRange,
                wantedRange)) {
            // close the file to get data up to date, it will be opened
            // again by read function
            fuseFileHandle->releaseHelperHandle(
                uuid, fileBlock.storageId(), fileBlock.fileId());

            LOG_DBG(2) << "Rereading the requested block from file " << uuid
                       << " due to mismatch in checksum - retries left: "
                       << retriesLeft;

            if (retriesLeft >= 0) {
                fiberRetryDelay(retriesLeft);

                LOG(INFO) << "Retrying read of " << size << " bytes at offset "
                          << offset << " from file " << uuid
                          << " - invalid checksum";

                // If this is a first retry with invalid checksum, force update
                // the file location map from the server for this file
                m_metadataCache.getLocation(uuid, true);

                if (m_ioTraceLoggerEnabled)
                    ioTraceEntry->retries++;

                return read(uuid, fileHandleId, offset, size, checksum,
                    retriesLeft - 1, std::move(ioTraceEntry));
            }

            LOG(ERROR) << "Failed to read " << size << " bytes at offset "
                       << offset << " from file " << uuid << " after "
                       << m_maxRetryCount << " retries"
                       << " - invalid checksum";

            throw std::system_error(std::make_error_code(std::errc::io_error));
        }

        const auto bytesRead = readBuffer.chainLength();
        if (!m_readEventsDisabled) {
            m_eventManager.emit<events::FileRead>(
                uuid.toStdString(), offset, bytesRead);
        }

        LOG_DBG(2) << "Read " << bytesRead << " bytes from " << uuid
                   << " at offset " << offset;

        // In case we have read less bytes that requested by user and
        if ((static_cast<int>(bytesRead) == 0) && (retriesLeft >= 0)) {
            LOG(INFO) << "Read only " << bytesRead
                      << " from storage instead of requested "
                      << boost::icl::length(wantedRange)
                      << " - retrying, retries left: " << retriesLeft;

            fiberRetryDelay(retriesLeft);
            m_metadataCache.getLocation(uuid, true);
            return read(uuid, fileHandleId, offset, size, checksum,
                retriesLeft - 1, std::move(ioTraceEntry));
        }

        if (m_ioTraceLoggerEnabled) {
            namespace sc = std::chrono;
            std::get<1>(ioTraceEntry->arguments) = bytesRead;
            ioTraceEntry->duration = sc::duration_cast<sc::microseconds>(
                sc::system_clock::now() - ioTraceEntry->timestamp);
            m_ioTraceLogger->log(*ioTraceEntry);
        }

        return readBuffer;
    }
    catch (const std::system_error &e) {
        // Folly fibers does not allow to recursively call from exception
        // handler Keep the error data, and retry after the catch block
        ec = e.code().value();
        ew = folly::exception_wrapper(std::current_exception(), e);
    }

    // Retry on error or rethrow exception
    if ((ec == EKEYEXPIRED) && (retriesLeft >= 0)) {
        auto defaultBlock = m_metadataCache.getDefaultBlock(uuid);
        auto storageId = defaultBlock.storageId();
        auto fileId = defaultBlock.fileId();
        LOG(INFO) << "Key or token to storage " << storageId
                  << " expired. Refreshing helper parameters...";

        auto spaceId = m_metadataCache.getSpaceId(uuid);

        folly::fibers::await([&](folly::fibers::Promise<folly::Unit> promise) {
            promise.setWith(
                [this, storageId, spaceId, fileId, fuseFileHandle, uuid]() {
                    // Invalidate the read cache so that it forgets
                    // the ekeyexpired exception
                    return m_helpersCache
                        ->refreshHelperParameters(storageId, spaceId)
                        .within(m_providerTimeout)
                        .get();
                });
        });

        return read(uuid, fileHandleId, offset, size, checksum, retriesLeft - 1,
            std::move(ioTraceEntry));
    }

    if ((ec == ENOENT) && (retriesLeft >= 0) &&
        !m_forceProxyIOCache.contains(uuid)) {
        // The file might have been moved on the storage - get the latest
        // file location

        // fiberRetryDelay(retriesLeft);
        m_metadataCache.getLocation(uuid, true);
        return read(uuid, fileHandleId, offset, size, checksum, retriesLeft - 1,
            std::move(ioTraceEntry));
    }

    if (((ec == EAGAIN) || (ec == ECANCELED)) && (retriesLeft >= 0)) {
        LOG_DBG(1) << "Retrying read due to error: " << ec;
        fiberRetryDelay(retriesLeft);
        return read(uuid, fileHandleId, offset, size, checksum, retriesLeft - 1,
            std::move(ioTraceEntry));
    }

    if ((ec != EPERM) && (ec != EACCES)) {
        LOG_DBG(1) << "Reading from " << uuid
                   << " failed due to error: " << ew.what() << "(" << ec << ")";
        ew.throw_exception();
    }

    LOG(ERROR) << "Reading from " << uuid
               << " failed due to insufficient permissions";

    if (m_forceProxyIOCache.contains(uuid)) {
        LOG(ERROR) << "Reading from " << uuid
                   << " failed since proxy mode is forced for this file";
        ew.throw_exception();
    }

    LOG_DBG(1) << "Adding file " << uuid
               << " to force proxy cache after direct read failed";

    m_forceProxyIOCache.add(uuid, true);

    LOG_DBG(1) << "Rereading requested block for " << uuid
               << " via proxy fallback, restarting retry counter";

    return read(uuid, fileHandleId, offset, size, checksum, m_maxRetryCount,
        std::move(ioTraceEntry));
}

std::pair<size_t, IOTraceLogger::PrefetchType> FsLogic::prefetchAsync(
    std::shared_ptr<FuseFileHandle> fuseFileHandle,
    const helpers::FileHandlePtr &helperHandle, const off_t offset,
    const std::size_t size, const folly::fbstring &uuid,
    const boost::icl::discrete_interval<off_t> possibleRange,
    const boost::icl::discrete_interval<off_t> availableRange)
{
    assertInFiber();

    size_t prefetchSize = 0;
    auto prefetchType = IOTraceLogger::PrefetchType::NONE;

    const auto fileSize = m_metadataCache.getAttr(uuid)->size().value_or(0);
    const auto fileLocation = m_metadataCache.getLocation(uuid);

    if (fileLocation->isReplicationComplete(fileSize))
        return {prefetchSize, prefetchType};

    const std::size_t wouldPrefetch = helperHandle->wouldPrefetch(offset, size);

    const auto wantToPrefetchRange =
        boost::icl::discrete_interval<off_t>::right_open(
            offset + size, offset + size + wouldPrefetch * 2);

    boost::icl::discrete_interval<off_t> prefetchRange{};
    bool worthPrefetching = false;
    bool clusterPrefetchRequested = false;
    int prefetchPriority = SYNCHRONIZE_BLOCK_PRIORITY_IMMEDIATE;

    if (m_randomReadPrefetchClusterWindow != 0) {
        off_t leftRange = 0;
        off_t rightRange = 0;
        bool blockAligned = false;

        // Make sure the prefetch is not calculated on each read
        if (!fuseFileHandle->shouldCalculatePrefetch())
            return {prefetchSize, prefetchType};

        LOG_DBG(2) << "Calculating random read prefetch condition for file "
                   << uuid;

        if (m_randomReadPrefetchClusterWindowGrowFactor == 0.0) {
            // Align the prefetch window to the consecutive block in the
            // file based on predefined prefetch block size
            const auto windowSize = m_randomReadPrefetchClusterWindow < 0
                ? fileSize
                : m_randomReadPrefetchClusterWindow;

            assert(windowSize > 0);

            leftRange = offset / windowSize;
            leftRange *= windowSize;
            rightRange = std::min<off_t>(leftRange + windowSize, fileSize);
            blockAligned = true;
        }
        else {
            // Calculate the current clustering window size based on initial
            // window size, grow factor and current replication progress
            const auto initialWindowSize = m_randomReadPrefetchClusterWindow < 0
                ? fileSize
                : m_randomReadPrefetchClusterWindow;

            const auto windowSize = static_cast<size_t>(initialWindowSize *
                (1.0 +
                    m_randomReadPrefetchClusterWindowGrowFactor * fileSize *
                        fileLocation->replicationProgress(fileSize) /
                        initialWindowSize));

            // Calculate a block range around the current read offset
            leftRange = std::max<off_t>(0, offset - windowSize / 2);
            rightRange = std::min<off_t>(offset + windowSize / 2, fileSize);
            blockAligned = false;
        }

        auto blocksInRange = fileLocation->blocksInRange(leftRange, rightRange);

        auto prefetchBlockThreshold = m_randomReadPrefetchClusterBlockThreshold;
        if (m_clusterPrefetchThresholdRandom) {
            prefetchBlockThreshold =
                m_clusterPrefetchDistribution(m_clusterPrefetchRandomGenerator);
        }

        LOG_DBG(2) << "Blocks in calculated prefetch range: " << blocksInRange
                   << ", threshold: " << prefetchBlockThreshold;

        if (blocksInRange > prefetchBlockThreshold) {
            if (blockAligned) {
                if (fuseFileHandle->prefetchAlreadyRequestedAt(leftRange)) {
                    LOG_DBG(2) << "Block aligned prefetch already "
                                  "requested at offset "
                               << leftRange << " - skipping prefetch";
                    return {0, IOTraceLogger::PrefetchType::NONE};
                }

                LOG_DBG(2) << "Block aligned prefetch at offset " << leftRange
                           << " not scheduled yet";

                fuseFileHandle->addPrefetchAt(leftRange);
            }

            LOG_DBG(1) << "Requesting clustered prefetch of block ["
                       << leftRange << ", " << rightRange << ") for file "
                       << uuid << ". " << blocksInRange
                       << " blocks in range (prefetch threshold: "
                       << prefetchBlockThreshold
                       << ", block aligned: " << blockAligned
                       << ", async: " << m_prefetchModeAsync << ")";

            prefetchRange = boost::icl::discrete_interval<off_t>::right_open(
                leftRange, rightRange);

            prefetchType = IOTraceLogger::PrefetchType::CLUSTER;
            prefetchPriority = SYNCHRONIZE_BLOCK_PRIORITY_CLUSTER_PREFETCH;

            worthPrefetching = true;
            clusterPrefetchRequested = true;
        }
    }

    if (m_linearReadPrefetchThreshold > 0.0 &&
        fileLocation->blocksLengthInRange(0,
            static_cast<std::size_t>(
                std::floor(fileSize * m_linearReadPrefetchThreshold))) >
            LINEAR_PREFETCH_THRESHOLD_MATCH_RATIO *
                std::floor(fileSize * m_linearReadPrefetchThreshold)) {
        worthPrefetching = true;
        prefetchRange =
            boost::icl::discrete_interval<off_t>::right_open(0, fileSize);
        LOG(INFO) << "Requesting linear prefetch for file " << uuid
                  << " in range " << prefetchRange
                  << " based on linear threshold "
                  << static_cast<std::size_t>(
                         std::floor(m_linearReadPrefetchThreshold * fileSize))
                  << "(async: " << m_prefetchModeAsync << ")";
    }
    else if (!clusterPrefetchRequested) {
        prefetchRange = boost::icl::left_subtract(
            wantToPrefetchRange & possibleRange, availableRange);

        LOG_DBG(2) << "Evaluating linear prefetch range: " << prefetchRange;
        LOG_DBG(2) << "Last prefetch was: " << fuseFileHandle->lastPrefetch();

        if (boost::icl::size(prefetchRange) > 0) {
            worthPrefetching = boost::icl::size(prefetchRange &
                                   fuseFileHandle->lastPrefetch()) == 0 ||
                boost::icl::size(boost::icl::left_subtract(
                    prefetchRange, fuseFileHandle->lastPrefetch())) >=
                    boost::icl::size(prefetchRange) / 2;

            if (worthPrefetching) {
                fuseFileHandle->setLastPrefetch(prefetchRange);
                LOG_DBG(1) << "Requesting linear prefetch for file " << uuid
                           << " in range " << prefetchRange
                           << "(async: " << m_prefetchModeAsync << ")";

                prefetchType = IOTraceLogger::PrefetchType::LINEAR;
                prefetchPriority = SYNCHRONIZE_BLOCK_PRIORITY_LINEAR_PREFETCH;
            }
        }
    }

    if (boost::icl::size(prefetchRange) > 0 && worthPrefetching) {
        prefetchSize = boost::icl::size(prefetchRange);
        // Request the calculated prefetch block, asynchronously or
        // synchronously depending on the command line flag
        if (m_prefetchModeAsync) {
            m_context->communicator()
                ->communicate<messages::fuse::FuseResponse>(
                    messages::fuse::BlockSynchronizationRequest{
                        uuid.toStdString(), prefetchRange, prefetchPriority,
                        false});
        }
        else {
            auto locationUpdate =
                communicate<messages::fuse::FileLocationChanged>(
                    messages::fuse::SynchronizeBlock{uuid.toStdString(),
                        prefetchRange, prefetchPriority, false},
                    m_providerTimeout);

            if (locationUpdate.changeStartOffset() &&
                locationUpdate.changeEndOffset())
                m_metadataCache.updateLocation(
                    *locationUpdate.changeStartOffset(),
                    *locationUpdate.changeEndOffset(),
                    locationUpdate.fileLocation());
            else
                m_metadataCache.updateLocation(locationUpdate.fileLocation());
        }
    }

    return {prefetchSize, prefetchType};
}

std::size_t FsLogic::write(const folly::fbstring &uuid,
    const std::uint64_t fuseFileHandleId, const off_t offset,
    std::shared_ptr<folly::IOBuf> buf, const int retriesLeft,
    std::unique_ptr<IOTraceWrite> ioTraceEntry)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(fuseFileHandleId)
                << LOG_FARG(offset) << LOG_FARG(buf->length());

    assertInFiber();

    if (buf->empty()) {
        LOG_DBG(2) << "Write called with empty buffer - skipping";
        return 0;
    }

    if (m_ioTraceLoggerEnabled && !ioTraceEntry) {
        ioTraceEntry = std::make_unique<IOTraceWrite>();
        ioTraceEntry->opType = IOTraceLogger::OpType::WRITE;
        ioTraceEntry->uuid = uuid;
        ioTraceEntry->handleId = fuseFileHandleId;
        ioTraceEntry->retries = 0;
        std::get<0>(ioTraceEntry->arguments) = offset;
        std::get<1>(ioTraceEntry->arguments) = 0;
    }

    if (!m_context->communicator()->isConnected()) {
        LOG(ERROR) << "Connection to Oneprovider lost...";
        if (retriesLeft > 0) {
            fiberRetryDelay(retriesLeft);
            LOG(INFO) << "Retrying write to " << uuid
                      << " - retries left: " << retriesLeft;
            return write(uuid, fuseFileHandleId, offset, std::move(buf),
                retriesLeft - 1, std::move(ioTraceEntry));
        }
        throw std::system_error(std::make_error_code(std::errc::timed_out));
    }

    if (m_fuseFileHandles.find(fuseFileHandleId) == m_fuseFileHandles.end()) {
        // File was probably opened during a network failure, reopen it
        // Get open flags from previous open
        auto flagIt = m_fuseFileHandleFlags.find(fuseFileHandleId);

        LOG(INFO) << "Reopening file " << uuid << " with descriptor "
                  << fuseFileHandleId << " after network connection failure...";

        if (flagIt == m_fuseFileHandleFlags.end()) {
            LOG(ERROR) << "Cannot reopen file " << uuid
                       << " - no cached open flags...";
            throw std::system_error(
                std::make_error_code(std::errc::bad_file_descriptor));
        }
        m_metadataCache.getAttr(uuid);
        open(uuid, flagIt->second, fuseFileHandleId);
    }

    auto fuseFileHandle = m_fuseFileHandles.at(fuseFileHandleId);
    auto attr = m_metadataCache.getAttr(uuid);
    auto spaceId = m_metadataCache.getSpaceId(uuid);

    // Check if this space is marked as disabled due to exceeded quota
    if (isSpaceDisabled(spaceId)) {
        LOG(ERROR) << "Write to file " << uuid << " failed - space "
                   << m_metadataCache.getSpaceId(uuid) << " quota exceeded";
        throw std::errc::no_space_on_device; // NOLINT
    }

    auto fileBlock = m_metadataCache.getDefaultBlock(uuid);

    using one::logging::log_timer;
    using one::logging::csv::log;
    using one::logging::csv::read_write_perf;

    size_t bytesWritten = 0;

    int ec = 0;
    folly::exception_wrapper ew;
    try {
        auto helperHandle = fuseFileHandle->getHelperHandle(
            uuid, spaceId, fileBlock.storageId(), fileBlock.fileId());

        folly::IOBufQueue bufq{folly::IOBufQueue::cacheChainLength()};
        bufq.append(buf->clone());

        log_timer<> timer;

        LOG_DBG(2) << "Writing to helper with timeout [ms]: "
                   << std::chrono::duration_cast<std::chrono::milliseconds>(
                          m_storageTimeout)
                          .count();
        bytesWritten = communication::wait(
            helperHandle->write(offset, std::move(bufq),
                [this, uuid = uuid.toStdString(), offset,
                    storageId = fileBlock.storageId(),
                    fileId = fileBlock.fileId()](std::size_t bytesWritten) {
                    m_eventManager.emit<events::FileWritten>(
                        uuid, offset, bytesWritten, storageId, fileId);
                }),
            m_storageTimeout);

        log<read_write_perf>(fileBlock.fileId(), "FsLogic", "write", offset,
            buf->length(), timer.stop());
    }
    catch (const std::system_error &e) {
        // Folly fibers does not allow to recursively call from exception
        // handler Keep the error data, and retry after the catch block
        ec = e.code().value();
        ew = folly::exception_wrapper(std::current_exception(), e);
    }

    if (ec != 0) {
        if ((ec == EKEYEXPIRED) && (retriesLeft >= 0)) {
            LOG_DBG(2) << "Key or token to storage " << fileBlock.storageId()
                       << " expired. Refreshing helper parameters...";

            folly::fibers::await(
                [&](folly::fibers::Promise<folly::Unit> promise) {
                    promise.setWith(
                        [this, storageId = fileBlock.storageId(),
                            spaceId = m_metadataCache.getSpaceId(uuid),
                            fuseFileHandle, uuid]() {
                            // Invalidate the read cache so that it forgets
                            // the ekeyexpired exception
                            return m_helpersCache
                                ->refreshHelperParameters(storageId, spaceId)
                                .within(m_providerTimeout)
                                .get();
                        });
                });

            return write(uuid, fuseFileHandleId, offset, std::move(buf),
                retriesLeft - 1, std::move(ioTraceEntry));
        }

        if ((ec == ENOENT) && (retriesLeft >= 0) &&
            !m_forceProxyIOCache.contains(uuid)) {
            // The file might have been moved on the storage - get the latest
            // file location
            fiberRetryDelay(retriesLeft);
            m_metadataCache.getLocation(uuid, true);
            return write(uuid, fuseFileHandleId, offset, std::move(buf),
                retriesLeft - 1, std::move(ioTraceEntry));
        }

        if ((ec == EAGAIN || ec == ETIMEDOUT) && (retriesLeft >= 0)) {
            fiberRetryDelay(retriesLeft);
            return write(uuid, fuseFileHandleId, offset, std::move(buf),
                retriesLeft - 1, std::move(ioTraceEntry));
        }

        if ((ec != EPERM) && (ec != EACCES)) {
            LOG(ERROR) << "Writing to " << uuid
                       << " failed with error code: " << ew.what();
            ew.throw_exception();
        }

        if (m_forceProxyIOCache.contains(uuid)) {
            LOG(ERROR) << "Writing to " << uuid
                       << " failed since proxy mode is forced for this file";
            ew.throw_exception();
        }

        LOG_DBG(1) << "Adding file " << uuid
                   << " to force proxy cache after direct write failed";

        m_forceProxyIOCache.add(uuid, true);

        LOG_DBG(1) << "Writing requested block for " << uuid
                   << " via proxy fallback";

        return write(uuid, fuseFileHandleId, offset, std::move(buf),
            retriesLeft, std::move(ioTraceEntry));
    }

    auto writtenRange = boost::icl::discrete_interval<off_t>::right_open(
        offset, offset + bytesWritten);

    LOG_DBG(2) << "Written " << bytesWritten << " bytes to file " << uuid
               << " at offset " << offset << " on storage "
               << fileBlock.storageId();

    m_metadataCache.addBlock(uuid, writtenRange, std::move(fileBlock));

    if (m_tagOnModify && !fuseFileHandle->isOnModifyTagSet()) {
        std::string tagNameJsonEncoded;
        std::string tagValueJsonEncoded;
        if (!util::xattr::encodeJsonXAttrName(
                m_tagOnModify.get().first, tagNameJsonEncoded) ||
            !util::xattr::encodeJsonXAttrValue(
                m_tagOnModify.get().second, tagValueJsonEncoded)) {
            LOG(ERROR) << "Setting on modify tag with invalid name or "
                          "value for file: "
                       << uuid;
        }
        else {
            FsLogic::setxattr(
                uuid, tagNameJsonEncoded, tagValueJsonEncoded, false, false);
        }
        fuseFileHandle->setOnModifyTag();
    }

    if (m_ioTraceLoggerEnabled) {
        std::get<1>(ioTraceEntry->arguments) = bytesWritten;
        namespace sc = std::chrono;
        ioTraceEntry->duration = sc::duration_cast<sc::microseconds>(
            sc::system_clock::now() - ioTraceEntry->timestamp);
        m_ioTraceLogger->log(*ioTraceEntry);
    }

    return bytesWritten;
}

FileAttrPtr FsLogic::mkdir(const folly::fbstring &parentUuid,
    const folly::fbstring &name, const mode_t mode)
{
    LOG_FCALL() << LOG_FARG(parentUuid) << LOG_FARG(name) << LOG_FARG(mode);

    IOTRACE_START()

    assertInFiber();

    // TODO: CreateDir should probably also return attrs
    communicate(messages::fuse::CreateDir{parentUuid.toStdString(),
                    name.toStdString(), mode},
        m_providerTimeout);

    LOG_DBG(2) << "Created directory " << name << " in " << parentUuid;

    // TODO: Provider returns uuid of the created dir, no need for lookup
    auto attr = m_metadataCache.getAttr(parentUuid, name);

    IOTRACE_END(IOTraceMkdir, IOTraceLogger::OpType::MKDIR, parentUuid, 0, name,
        attr->uuid(), mode)

    return attr;
}

FileAttrPtr FsLogic::mknod(const folly::fbstring &parentUuid,
    const folly::fbstring &name, const mode_t mode)
{
    LOG_FCALL() << LOG_FARG(parentUuid) << LOG_FARG(name) << LOG_FARG(mode);

    IOTRACE_START()

    assertInFiber();

    if (S_ISDIR(mode) || S_ISCHR(mode) || S_ISBLK(mode) || S_ISFIFO(mode) ||
        S_ISLNK(mode) || S_ISSOCK(mode)) {
        LOG(ERROR) << "Attempt to create unsupported node type - only regular "
                      "files are supported for this call.";
        throw std::errc::operation_not_supported; // NOLINT
    }

    constexpr auto modeMask =
        S_ISUID | S_ISGID | S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO;
    messages::fuse::MakeFile msg{parentUuid, name, mode & modeMask};
    auto attr = communicate<FileAttr>(std::move(msg), m_providerTimeout);

    LOG_DBG(2) << "Created node " << name << " in " << parentUuid
               << " with uuid " << attr.uuid();

    auto sharedAttr = std::make_shared<FileAttr>(std::move(attr));
    m_metadataCache.putAttr(sharedAttr);

    IOTRACE_END(IOTraceMknod, IOTraceLogger::OpType::MKNOD, parentUuid, 0, name,
        sharedAttr->uuid(), mode)

    return sharedAttr;
}

FileAttrPtr FsLogic::link(const folly::fbstring &uuid,
    const folly::fbstring &newParentUuid, const folly::fbstring &newName)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(newParentUuid)
                << LOG_FARG(newName);

    IOTRACE_START()

    assertInFiber();

    messages::fuse::MakeLink msg{uuid, newParentUuid, newName};
    auto attr = communicate<FileAttr>(std::move(msg), m_providerTimeout);
    auto sharedAttr = std::make_shared<FileAttr>(std::move(attr));

    m_metadataCache.putAttr(sharedAttr);

    IOTRACE_END(IOTraceLink, IOTraceLogger::OpType::LINK, uuid, 0,
        newParentUuid, newName)

    return sharedAttr;
}

FileAttrPtr FsLogic::symlink(const folly::fbstring &parentUuid,
    const folly::fbstring &name, const folly::fbstring &link)
{
    LOG_FCALL() << LOG_FARG(link) << LOG_FARG(parentUuid) << LOG_FARG(name);

    assertInFiber();

    IOTRACE_START()

    folly::fbstring effectiveLink{link};
    if (!effectiveLink.empty() && (effectiveLink[0] == '/')) {
        effectiveLink = createSpaceRelativeSymlink(effectiveLink);

        LOG_DBG(2) << "Creating space-relative absolute symlink: "
                   << effectiveLink;
    }

    messages::fuse::MakeSymLink msg{parentUuid, name, effectiveLink};
    auto attr = communicate<FileAttr>(std::move(msg), m_providerTimeout);
    auto sharedAttr = std::make_shared<FileAttr>(std::move(attr));

    m_metadataCache.putAttr(sharedAttr);

    IOTRACE_END(
        IOTraceLink, IOTraceLogger::OpType::SYMLINK, parentUuid, 0, name, link)

    return sharedAttr;
}

folly::fbstring FsLogic::readlink(const folly::fbstring &uuid)
{
    LOG_FCALL() << LOG_FARG(uuid);

    assertInFiber();

    IOTRACE_GUARD(IOTraceReadLink, IOTraceLogger::OpType::READLINK, uuid, 0)

    messages::fuse::ReadSymLink msg{uuid};
    auto symlink = communicate<one::messages::fuse::SymLink>(
        std::move(msg), m_providerTimeout);

    if (symlink.link().find(kAbsLinkPrefix) == 0) {
        // This is space-relative absolute symlink
        return resolveSpaceRelativeSymlink(symlink.link());
    }

    return symlink.link();
}

std::pair<FileAttrPtr, std::uint64_t> FsLogic::create(
    const folly::fbstring &parentUuid, const folly::fbstring &name,
    const mode_t mode, const int flags)
{
    LOG_FCALL() << LOG_FARG(parentUuid) << LOG_FARG(name) << LOG_FARG(mode)
                << LOG_FARG(flags);

    IOTRACE_START()

    assertInFiber();

    if (S_ISDIR(mode) || S_ISCHR(mode) || S_ISBLK(mode) || S_ISFIFO(mode) ||
        S_ISLNK(mode) || S_ISSOCK(mode)) {

        LOG(ERROR) << "Attempt to create unsupported file type - only regular "
                      "files are supported for this call.";
        throw std::errc::operation_not_supported; // NOLINT
    }

    constexpr auto modeMask =
        S_ISUID | S_ISGID | S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO;
    const auto flag = getOpenFlag(helpers::maskToFlags(flags));
    messages::fuse::CreateFile msg{parentUuid, name, mode & modeMask, flag};

    auto created = communicate<messages::fuse::FileCreated>(
        std::move(msg), m_providerTimeout);

    const auto &uuid = created.attr().uuid();
    auto sharedAttr = std::make_shared<FileAttr>(created.attr());
    auto location = std::make_unique<FileLocation>(created.location());
    auto openFileToken =
        m_metadataCache.open(uuid, sharedAttr, std::move(location));

    const auto fuseFileHandleId = m_nextFuseHandleId++;

    auto fuseFileHandle = std::make_shared<FuseFileHandle>(flags,
        created.handleId(), openFileToken, *m_helpersCache, m_forceProxyIOCache,
        m_providerTimeout);

    m_fuseFileHandles.emplace(fuseFileHandleId, fuseFileHandle);
    m_openFileHandles.emplace(uuid, fuseFileHandleId);
    m_fuseFileHandleFlags.emplace(fuseFileHandleId, flags);

    LOG_DBG(2) << "Created file " << name << " in " << parentUuid
               << " with uuid " << uuid;

    if (m_tagOnCreate && !fuseFileHandle->isOnCreateTagSet()) {
        std::string tagNameJsonEncoded;
        std::string tagValueJsonEncoded;
        if (!util::xattr::encodeJsonXAttrName(
                m_tagOnCreate.get().first, tagNameJsonEncoded) ||
            !util::xattr::encodeJsonXAttrValue(
                m_tagOnCreate.get().second, tagValueJsonEncoded)) {
            LOG(ERROR) << "Setting on create tag with invalid name or "
                          "value for file "
                       << uuid;
        }
        else {
            FsLogic::setxattr(uuid, m_tagOnCreate.get().first,
                tagValueJsonEncoded, false, false);
        }
        fuseFileHandle->setOnCreateTag();
    }

    IOTRACE_END(IOTraceCreate, IOTraceLogger::OpType::CREATE, parentUuid,
        fuseFileHandleId, name, sharedAttr->uuid(), mode & modeMask, flags)

    return {sharedAttr, fuseFileHandleId};
}

void FsLogic::unlink(
    const folly::fbstring &parentUuid, const folly::fbstring &name)
{
    LOG_FCALL() << LOG_FARG(parentUuid) << LOG_FARG(name);

    IOTRACE_START()

    assertInFiber();

    // TODO: directly order provider to delete {parentUuid, name}
    FileAttrPtr attr{};
    auto fileNameUUID = getFileIdFromFilename(name);
    if (fileNameUUID.empty())
        attr = m_metadataCache.getAttr(parentUuid, name);
    else
        attr = m_metadataCache.getAttr(fileNameUUID);

    try {
        communicate(messages::fuse::DeleteFile{attr->uuid().toStdString()},
            m_providerTimeout);
    }
    catch (std::system_error &e) {
        LOG_DBG(1) << e.what();
        if (e.code().value() == ENOENT) {
            LOG_DBG(1) << "File or directory " << name << " in parent "
                       << parentUuid << " doesn't exist";
            m_metadataCache.markDeleted(attr->uuid());
        }
        throw e;
    }

    m_metadataCache.markDeleted(attr->uuid());

    IOTRACE_END(IOTraceUnlink, IOTraceLogger::OpType::UNLINK, parentUuid, 0,
        name, attr->uuid())

    LOG_DBG(2) << "Deleted file or directory " << name << " in " << parentUuid
               << " with uuid " << attr->uuid();
}

void FsLogic::rename(const folly::fbstring &parentUuid,
    const folly::fbstring &name, const folly::fbstring &newParentUuid,
    const folly::fbstring &newName)
{
    LOG_FCALL() << LOG_FARG(parentUuid) << LOG_FARG(name)
                << LOG_FARG(newParentUuid) << LOG_FARG(newName);

    IOTRACE_START()

    assertInFiber();

    // TODO: directly order provider to rename {parentUuid, name}
    FileAttrPtr attr{};
    auto fileNameUUID = getFileIdFromFilename(name);
    if (fileNameUUID.empty())
        attr = m_metadataCache.getAttr(parentUuid, name);
    else
        attr = m_metadataCache.getAttr(fileNameUUID);

    auto oldUuid = attr->uuid();

    auto renamed = communicate<messages::fuse::FileRenamed>(
        messages::fuse::Rename{oldUuid.toStdString(),
            newParentUuid.toStdString(), newName.toStdString()},
        m_providerTimeout);

    m_metadataCache.rename(oldUuid, newParentUuid, newName, renamed.newUuid());

    LOG_DBG(2) << "Renamed file " << name << " in " << parentUuid << " to "
               << newName << " in " << newParentUuid;

    for (const auto &child : renamed.childEntries())
        m_metadataCache.rename(child.oldUuid(), child.newParentUuid(),
            child.newName(), child.newUuid());

    IOTRACE_END(IOTraceRename, IOTraceLogger::OpType::RENAME, parentUuid, 0,
        name, oldUuid, newParentUuid, newName, renamed.newUuid())
}

std::map<folly::fbstring, folly::fbvector<std::pair<off_t, off_t>>>
FsLogic::getFileLocalBlocks(const folly::fbstring &uuid)
{
    return m_metadataCache.getLocation(uuid)->getFileLocalBlocks();
}

FileAttrPtr FsLogic::setattr(
    const folly::fbstring &uuid, const struct stat &attr, const int toSet)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(toSet);

    IOTRACE_GUARD(IOTraceSetAttr, IOTraceLogger::OpType::SETATTR, uuid, 0,
        toSet, attr.st_mode, attr.st_size, attr.st_atime, attr.st_mtime)

    assertInFiber();

    if ((toSet & FUSE_SET_ATTR_UID) != 0 || (toSet & FUSE_SET_ATTR_GID) != 0) {
        LOG_DBG(2) << "Attempting to modify uid or gid attempted for " << uuid
                   << ". Operation not supported - ignoring...";
    }

    if ((toSet & FUSE_SET_ATTR_MODE) != 0) {
        // ALLPERMS is a macro of sys/stat.h
        const mode_t normalizedMode = attr.st_mode & ALLPERMS;

        communicate(
            messages::fuse::ChangeMode{uuid.toStdString(), normalizedMode},
            m_providerTimeout);

        m_metadataCache.changeMode(uuid, normalizedMode);

        LOG_DBG(2) << "Changed mode of " << uuid << " to "
                   << LOG_OCT(normalizedMode);
    }

    if ((toSet & FUSE_SET_ATTR_SIZE) != 0) {
        // Make sure all opened handles for file uuid are fsynced before
        // truncating
        auto pairIt = m_openFileHandles.equal_range(uuid);
        auto it = pairIt.first;
        for (; it != pairIt.second; ++it) {
            auto fileHandleId = it->second;
            flush(uuid, fileHandleId);
        }

        m_eventManager.flush();

        communicate(messages::fuse::Truncate{uuid.toStdString(), attr.st_size},
            m_providerTimeout);
        m_metadataCache.truncate(uuid, attr.st_size);
        m_eventManager.emit<events::FileTruncated>(
            uuid.toStdString(), attr.st_size);

        LOG_DBG(2) << "Truncated file " << uuid << " to size " << attr.st_size
                   << " via setattr";

        ONE_METRIC_COUNTER_INC(
            "comp.oneclient.mod.events.submod.emitted.truncate");
    }

    messages::fuse::UpdateTimes updateTimes{uuid.toStdString()};

    const auto now = std::chrono::system_clock::now();
    updateTimes.ctime(now);
    if ((toSet & FUSE_SET_ATTR_ATIME) != 0) {
        updateTimes.atime(
            std::chrono::system_clock::from_time_t(attr.st_atime));
        LOG_DBG(2) << "Changed atime of " << uuid << " to " << attr.st_atime;
    }
    if ((toSet & FUSE_SET_ATTR_MTIME) != 0) {
        // Make sure all opened handles for file uuid are fsynced before
        // updating mtime
        auto pairIt = m_openFileHandles.equal_range(uuid);
        auto it = pairIt.first;
        for (; it != pairIt.second; ++it) {
            auto fileHandleId = it->second;
            flush(uuid, fileHandleId);
        }

        m_eventManager.flush();

        updateTimes.mtime(
            std::chrono::system_clock::from_time_t(attr.st_mtime));
        LOG_DBG(2) << "Changed mtime of " << uuid << " to " << attr.st_atime;
    }
#if defined(FUSE_SET_ATTR_ATIME_NOW)
    if ((toSet & FUSE_SET_ATTR_ATIME_NOW) != 0) {
        updateTimes.atime(now);
        LOG_DBG(2) << "Changed atime of " << uuid << " to now";
    }
#endif
#if defined(FUSE_SET_ATTR_MTIME_NOW)
    if ((toSet & FUSE_SET_ATTR_MTIME_NOW) != 0) {
        updateTimes.mtime(now);
        LOG_DBG(2) << "Changed mtime of " << uuid << " to now";
    }
#endif

    communicate(updateTimes, m_providerTimeout);
    m_metadataCache.updateTimes(uuid, updateTimes);

    return m_metadataCache.getAttr(uuid);
}

folly::fbstring FsLogic::getxattr(
    const folly::fbstring &uuid, const folly::fbstring &name)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(name);

    IOTRACE_GUARD(
        IOTraceGetXAttr, IOTraceLogger::OpType::GETXATTR, uuid, 0, name)

    assertInFiber();

    folly::fbstring result;

    if (name == ONE_XATTR("guid")) {
        return folly::sformat("\"{}\"", uuid);
    }

    if (name == ONE_XATTR("file_id")) {
        return folly::sformat(
            "\"{}\"", util::cdmi::uuidToObjectId(uuid.toStdString()));
    }

    if (name == ONE_XATTR("storage_file_id")) {
        return folly::sformat(
            "\"{}\"", m_metadataCache.getDefaultBlock(uuid).fileId());
    }

    if (name == ONE_XATTR("storage_id")) {
        return folly::sformat(
            "\"{}\"", m_metadataCache.getDefaultBlock(uuid).storageId());
    }

    if (name == ONE_XATTR("space_id")) {
        return folly::sformat(
            "\"{}\"", util::uuid::uuidToSpaceId(uuid).toStdString());
    }

    if (name == ONE_XATTR("access_type")) {
        auto accessType = m_helpersCache->getAccessType(
            m_metadataCache.getDefaultBlock(uuid).storageId());

        if (accessType == cache::HelpersCache::AccessType::DIRECT)
            return "\"direct\"";

        if (accessType == cache::HelpersCache::AccessType::PROXY)
            return "\"proxy\"";

        return "\"unknown\"";
    }

    if (name == ONE_XATTR("file_blocks_count")) {
        auto forceLocationUpdate =
            !m_fsSubscriptions.isSubscribedToFileLocationChanged(uuid);
        return folly::sformat("\"{}\"",
            m_metadataCache.getLocation(uuid, forceLocationUpdate)
                ->blocksCount());
    }

    if (name == ONE_XATTR("file_blocks")) {
        std::size_t size = m_metadataCache.getAttr(uuid)->size().value_or(0);
        if (size == 0) {
            return "\"empty\"";
        }

        auto forceLocationUpdate =
            !m_fsSubscriptions.isSubscribedToFileLocationChanged(uuid);
        return folly::sformat("\"[{}]\"",
            m_metadataCache.getLocation(uuid, forceLocationUpdate)
                ->progressString(size, XATTR_FILE_BLOCKS_MAP_LENGTH));
    }

    if (name == ONE_XATTR("replication_progress")) {
        std::size_t size = m_metadataCache.getAttr(uuid)->size().value_or(0);

        auto forceLocationUpdate =
            !m_fsSubscriptions.isSubscribedToFileLocationChanged(uuid);
        auto replicationProgress =
            m_metadataCache.getLocation(uuid, forceLocationUpdate)
                ->replicationProgress(size);

        constexpr auto REPLICATION_PROGRESS_TO_PERCENT = 100;
        return folly::sformat("\"{}%\"",
            static_cast<int>(std::floor(
                replicationProgress * REPLICATION_PROGRESS_TO_PERCENT)));
    }

    messages::fuse::GetXAttr getXAttrRequest{uuid, name};
    auto xattr =
        communicate<messages::fuse::XAttr>(getXAttrRequest, m_providerTimeout);
    result = xattr.value();

    LOG_DBG(2) << "Received xattr " << name << " value for file " << uuid;

    return result;
}

void FsLogic::setxattr(const folly::fbstring &uuid, const folly::fbstring &name,
    const folly::fbstring &value, bool create, bool replace)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(name) << LOG_FARG(value)
                << LOG_FARG(create) << LOG_FARG(replace);

    IOTRACE_GUARD(IOTraceSetXAttr, IOTraceLogger::OpType::SETXATTR, uuid, 0,
        name, value, create, replace)

    assertInFiber();

    messages::fuse::SetXAttr setXAttrRequest{
        uuid, name, value, create, replace};
    communicate<messages::fuse::FuseResponse>(
        setXAttrRequest, m_providerTimeout);

    LOG_DBG(2) << "Set xattr " << name << " value for file " << uuid;
}

void FsLogic::removexattr(
    const folly::fbstring &uuid, const folly::fbstring &name)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(name);

    IOTRACE_GUARD(
        IOTraceRemoveXAttr, IOTraceLogger::OpType::REMOVEXATTR, uuid, 0, name)

    assertInFiber();

    messages::fuse::RemoveXAttr removeXAttrRequest{uuid, name};
    communicate<messages::fuse::FuseResponse>(
        removeXAttrRequest, m_providerTimeout);

    LOG_DBG(2) << "Removed xattr " << name << " from file " << uuid;
}

folly::fbvector<folly::fbstring> FsLogic::listxattr(const folly::fbstring &uuid)
{
    LOG_FCALL() << LOG_FARG(uuid);

    IOTRACE_GUARD(IOTraceListXAttr, IOTraceLogger::OpType::LISTXATTR, uuid, 0)

    assertInFiber();

    namespace omf = one::messages::fuse;

    folly::fbvector<folly::fbstring> result;

    omf::ListXAttr listXAttrRequest{uuid};
    omf::XAttrList fuseResponse =
        communicate<omf::XAttrList>(listXAttrRequest, m_providerTimeout);

    for (const auto &xattrName : fuseResponse.xattrNames()) {
        result.push_back(xattrName.c_str());
    }

    result.push_back(ONE_XATTR("guid"));
    result.push_back(ONE_XATTR("file_id"));
    result.push_back(ONE_XATTR("space_id"));

    auto fileType = m_metadataCache.getAttr(uuid)->type();
    if ((fileType == FileAttr::FileType::regular) ||
        (fileType == FileAttr::FileType::link)) {
        result.push_back(ONE_XATTR("storage_id"));
        result.push_back(ONE_XATTR("storage_file_id"));
        result.push_back(ONE_XATTR("access_type"));
        result.push_back(ONE_XATTR("file_blocks"));
        result.push_back(ONE_XATTR("file_blocks_count"));
        result.push_back(ONE_XATTR("replication_progress"));
    }

    LOG_DBG(2) << "Received xattr list for file " << uuid;

    return result;
}

template <typename SrvMsg, typename CliMsg>
SrvMsg FsLogic::communicate(CliMsg &&msg, const std::chrono::seconds timeout)
{
    auto messageString = msg.toString();
    return m_context->communicator()
        ->communicate<SrvMsg>(std::forward<CliMsg>(msg))
        .onTimeout(timeout,
            [messageString = std::move(messageString),
                timeout = timeout.count()]() {
                LOG(ERROR) << "Response to message : " << messageString
                           << " not received within " << timeout << " seconds.";
                return folly::makeFuture<SrvMsg>(std::system_error{
                    std::make_error_code(std::errc::timed_out)});
            })
        .get();
}

folly::fbstring FsLogic::syncAndFetchChecksum(const folly::fbstring &uuid,
    const boost::icl::discrete_interval<off_t> &range)
{
    messages::fuse::SynchronizeBlockAndComputeChecksum request{
        uuid.toStdString(), range, SYNCHRONIZE_BLOCK_PRIORITY_IMMEDIATE};

    auto syncResponse = communicate<messages::fuse::SyncResponse>(
        std::move(request), m_providerTimeout);

    const auto &fileLocationUpdate = syncResponse.fileLocationChanged();
    if (fileLocationUpdate.changeStartOffset() &&
        fileLocationUpdate.changeEndOffset())
        m_metadataCache.updateLocation(
            *(fileLocationUpdate.changeStartOffset()),
            *(fileLocationUpdate.changeEndOffset()),
            fileLocationUpdate.fileLocation());
    else
        m_metadataCache.updateLocation(fileLocationUpdate.fileLocation());

    return syncResponse.checksum();
}

void FsLogic::sync(const folly::fbstring &uuid,
    const boost::icl::discrete_interval<off_t> &range)
{
    messages::fuse::SynchronizeBlock request{
        uuid.toStdString(), range, SYNCHRONIZE_BLOCK_PRIORITY_IMMEDIATE, false};
    auto fileLocationUpdate = communicate<messages::fuse::FileLocationChanged>(
        std::move(request), m_providerTimeout);

    if (fileLocationUpdate.fileLocation().uuid() != uuid) {
        const auto &fetchedUuid = fileLocationUpdate.fileLocation().uuid();
        // Check if this is the same file in case uuid is a share id
        if ((util::uuid::uuidToSpaceId(fetchedUuid) ==
                util::uuid::uuidToSpaceId(uuid)) &&
            (util::uuid::uuidToGuid(fetchedUuid) ==
                util::uuid::uuidToGuid(uuid))) {
            fileLocationUpdate.setUuid(uuid);
        }
        else {
            LOG(ERROR) << "Synchronize block request for file " << uuid
                       << "returned file location for different uuid "
                       << fileLocationUpdate.fileLocation().uuid();
        }
    }

    if (fileLocationUpdate.changeStartOffset() &&
        fileLocationUpdate.changeEndOffset())
        m_metadataCache.updateLocation(
            *(fileLocationUpdate.changeStartOffset()),
            *(fileLocationUpdate.changeEndOffset()),
            fileLocationUpdate.fileLocation());
    else
        m_metadataCache.updateLocation(fileLocationUpdate.fileLocation());
}

bool FsLogic::dataCorrupted(const folly::fbstring &uuid,
    const folly::IOBufQueue &buf, const folly::fbstring &serverChecksum,
    const boost::icl::discrete_interval<off_t> &availableRange,
    const boost::icl::discrete_interval<off_t> &wantedRange)
{
    if (availableRange == wantedRange) {
        auto computedHash = computeHash(buf);
        if (computedHash != serverChecksum) {
            std::vector<char> computedVec(
                computedHash.begin(), computedHash.end());
            std::vector<char> serverVec(
                serverChecksum.begin(), serverChecksum.end());
            LOG_DBG(1) << "Checksum mismatch for file " << uuid
                       << ", expected hash '" << LOG_ERL_BIN(serverVec)
                       << "' - read '" << LOG_ERL_BIN(computedVec);
            return true;
        }

        return false;
    }

    LOG(ERROR) << "Requested data corruption check for file " << uuid
               << " on invalid range: " << wantedRange;

    return true;
}

folly::fbstring FsLogic::computeHash(const folly::IOBufQueue &buf)
{
    LOG_FCALL() << LOG_FARG(buf.chainLength());
    // TODO: move this to CPU-bound threadpool
    return folly::fibers::await(
        [&](folly::fibers::Promise<folly::fbstring> promise) {
            m_context->scheduler()->post([&,
                                             promise =
                                                 std::move(promise)]() mutable {
                folly::fbstring hash(MD4_DIGEST_LENGTH, '\0');
                MD4_CTX ctx;
                MD4_Init(&ctx);

                if (!buf.empty())
                    for (const auto &byteRange : *buf.front())
                        MD4_Update(&ctx, byteRange.data(), byteRange.size());

                // NOLINTNEXTLINE
                MD4_Final(reinterpret_cast<unsigned char *>(&hash[0]), &ctx);
                promise.setValue(std::move(hash));
            });
        });
}

folly::fbstring FsLogic::getFileIdFromFilename(const folly::fbstring &name)
{
    if (name.find(ONEDATA_FILEID_ACCESS_PREFIX) == 0) {
        return util::cdmi::objectIdToUUID(
            name.substr(strlen(ONEDATA_FILEID_ACCESS_PREFIX)).toStdString());
    }

    return {};
}

bool FsLogic::isSpaceDisabled(const folly::fbstring &spaceId)
{
    return m_disabledSpaces.count(spaceId) > 0;
}

void FsLogic::disableSpaces(const std::vector<std::string> &spaces)
{
    m_disabledSpaces = {spaces.begin(), spaces.end()};
}

void FsLogic::pruneExpiredDirectories(const std::chrono::seconds delay)
{
    // When user sets this option to 0, directories should never expire
    if (delay.count() == 0)
        return;

    while (true) {
        m_directoryCachePruneBaton.reset();
        m_directoryCachePruneBaton.timed_wait(delay);

        if (m_stopped)
            break;

        LOG_DBG(2) << "Running scheduled pruning of expired entries from "
                      "directory cache...";

        m_metadataCache.pruneExpiredDirectories();
    }
}

void FsLogic::fiberRetryDelay(int retriesLeft)
{
    assertInFiber();

    const auto retryIndex = std::min(
        std::max(0, m_maxRetryCount - retriesLeft), m_maxRetryCount - 1);

    auto delayRange = FSLOGIC_RETRY_DELAYS.at(retryIndex);
    auto delay = std::chrono::milliseconds(delayRange.first +
        (std::rand() % (delayRange.second - delayRange.first + 1))); // NOLINT

    LOG(INFO) << "Retrying FsLogic operation after " << delay.count()
              << "ms. Retries left: " << retriesLeft;

    folly::fibers::Baton baton;
    baton.timed_wait(delay);
}

std::shared_ptr<IOTraceLogger> FsLogic::createIOTraceLogger()
{
    auto now = std::chrono::system_clock::now();
    auto nowTimeT = std::chrono::system_clock::to_time_t(now);
    constexpr auto IOTRACE_TIME_BUFFER_SIZE = 512U;
    char nowBuf[IOTRACE_TIME_BUFFER_SIZE];

    std::tm nowTm = *std::localtime(&nowTimeT);
    std::strftime(nowBuf, IOTRACE_TIME_BUFFER_SIZE, "%Y%m%dT%H%M%S", &nowTm);
    auto traceFilePath = m_context->options()->getLogDirPath() /
        (std::string{"iotrace-"} + nowBuf + ".csv");

    return IOTraceLogger::make(traceFilePath.native());
}

folly::fbstring FsLogic::resolveSpaceRelativeSymlink(
    const folly::fbstring &link)
{
    auto spaceId = link.substr(kAbsLinkPrefix.size());
    if (spaceId.find('>') == std::string::npos)
        return link;

    auto prefixEnd = spaceId.find('>', 0);
    auto relativePath = spaceId.substr(prefixEnd + 1);
    if (!relativePath.empty() && relativePath[0] != '/')
        relativePath = "/" + relativePath;
    spaceId = spaceId.substr(0, spaceId.find('>', 0));

    auto spaceUuid = util::uuid::spaceIdToSpaceUUID(spaceId);

    try {
        auto attr = m_metadataCache.getAttr(spaceUuid);
        auto mountPoint = boost::filesystem::absolute(
            m_context->options()->getMountpoint(), "/");

        auto mountPointString = mountPoint.string();
        if (mountPointString.back() == '/')
            mountPointString.pop_back();

        if (m_showSpaceIdsNotNames)
            return fmt::format(
                "{}/{}{}", mountPointString, spaceId, relativePath);

        auto absLink = fmt::format(
            "{}/{}{}", mountPointString, attr->name(), relativePath);

        LOG_DBG(2) << "Return space-relative absolute link: " << absLink;

        return absLink;
    }
    catch (boost::filesystem::filesystem_error &e) {
        return link;
    }
    catch (std::system_error &e) {
        if (e.code().value() == ENOENT)
            return link;
        throw;
    }
}

folly::fbstring FsLogic::createSpaceRelativeSymlink(const folly::fbstring &link)
{
    folly::fbstring effectiveLink{link};
    try {
        auto mountPoint = boost::filesystem::absolute(
            m_context->options()->getMountpoint(), "/");

        if (effectiveLink.back() == '/')
            effectiveLink.pop_back();

        if (effectiveLink.find(mountPoint.string()) == 0) {
            // Get space name from the path
            auto pathRelativeToMountpoint = boost::filesystem::path{
                effectiveLink.substr(mountPoint.string().size()).toStdString()};

            if (pathRelativeToMountpoint.string().size() > 1) {
                auto spaceName =
                    *pathRelativeToMountpoint.relative_path().begin();

                auto attr =
                    m_metadataCache.getAttr(m_rootUuid, spaceName.string());

                auto spacePath = mountPoint.string();
                if (spacePath.back() == '/')
                    spacePath += spaceName.string();
                else
                    spacePath += std::string("/") + spaceName.string();

                auto spaceRelativePath = effectiveLink.substr(spacePath.size());

                if (!spaceRelativePath.empty()) {
                    if (spaceRelativePath[0] == '/')
                        spaceRelativePath.erase(spaceRelativePath.begin());

                    effectiveLink = fmt::format("{}{}>/{}", kAbsLinkPrefix,
                        util::uuid::uuidToSpaceId(attr->uuid()).toStdString(),
                        spaceRelativePath);
                }
                else {
                    effectiveLink = fmt::format("{}{}>", kAbsLinkPrefix,
                        util::uuid::uuidToSpaceId(attr->uuid()).toStdString());
                }
            }
        }
    }
    catch (boost::filesystem::filesystem_error &e) {
    }
    catch (std::system_error &e) {
        if (e.code().value() != ENOENT)
            throw;
    }

    return effectiveLink;
}
} // namespace fslogic
} // namespace client
} // namespace one
