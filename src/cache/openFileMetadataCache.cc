#include "openFileMetadataCache.h"

#include "cache/readdirCache.h"
#include "helpers/logging.h"
#include "messages/fuse/fileAttr.h"
#include "messages/fuse/fileLocation.h"

#include <functional>

namespace one {
namespace client {
namespace cache {

OpenFileMetadataCache::OpenFileToken::OpenFileToken(
    FileAttrPtr attr, OpenFileMetadataCache &cache)
    : m_attr{std::move(attr)}
    , m_cache{cache}
{
}

OpenFileMetadataCache::OpenFileToken::~OpenFileToken()
{
    m_cache.releaseFile(m_attr->uuid());
}

OpenFileMetadataCache::OpenFileMetadataCache(
    communication::Communicator &communicator, const std::size_t targetSize,
    const std::chrono::seconds providerTimeout,
    const std::chrono::seconds directoryCacheDropAfter,
    const folly::fbstring &rootUuid, const std::vector<std::string> &spaceNames,
    const std::vector<std::string> &spaceIds)
    : MetadataCache{communicator, providerTimeout, rootUuid, spaceNames,
          spaceIds}
    , m_targetSize{targetSize}
    , m_directoryCacheDropAfter{directoryCacheDropAfter}
{
    MetadataCache::onRename(std::bind(&OpenFileMetadataCache::handleRename,
        this, std::placeholders::_1, std::placeholders::_2));

    MetadataCache::onMarkDeleted(
        std::bind(&OpenFileMetadataCache::handleMarkDeleted, this,
            std::placeholders::_1));
}

void OpenFileMetadataCache::setReaddirCache(
    std::shared_ptr<ReaddirCache> readdirCache)
{
    MetadataCache::setReaddirCache(readdirCache);
}

bool OpenFileMetadataCache::isDirectorySynced(const folly::fbstring &uuid)
{
    assertInFiber();

    auto it = m_lruDirectoryData.find(uuid);
    if (it == m_lruDirectoryData.end())
        return false;

    return it->second.dirRead;
}

void OpenFileMetadataCache::setDirectorySynced(const folly::fbstring &uuid)
{
    assertInFiber();

    noteDirectoryActivity(uuid);
    auto it = m_lruDirectoryData.find(uuid);

    assert(it != m_lruDirectoryData.end());

    if (it != m_lruDirectoryData.end())
        it->second.dirRead = true;

    m_onSyncDirectory(uuid);
}

void OpenFileMetadataCache::opendir(const folly::fbstring &uuid)
{
    LOG_FCALL() << LOG_FARG(uuid);

    assertInFiber();

    noteDirectoryActivity(uuid);
    auto it = m_lruDirectoryData.find(uuid);

    assert(it != m_lruDirectoryData.end());

    it->second.openCount++;
}

void OpenFileMetadataCache::releasedir(const folly::fbstring &uuid)
{
    LOG_FCALL() << LOG_FARG(uuid);

    assertInFiber();

    auto res = m_lruDirectoryData.emplace(uuid, OpenFileData{});

    auto &lruData = res.first->second;

    if (lruData.openCount > 0)
        lruData.openCount--;

    if (lruData.openCount == 0 && lruData.deleted) {
        if (lruData.lruIt)
            m_lruDirectoryList.erase(*lruData.lruIt);

        m_lruDirectoryData.erase(uuid);
    }

    prune();
}

folly::fbvector<folly::fbstring> OpenFileMetadataCache::readdir(
    const folly::fbstring &uuid, off_t off, std::size_t chunkSize,
    bool includeVirtual, bool onlyFullReplicas)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(off) << LOG_FARG(chunkSize);

    assertInFiber();

    noteDirectoryActivity(uuid);

    return MetadataCache::readdir(
        uuid, off, chunkSize, includeVirtual, onlyFullReplicas);
}

void OpenFileMetadataCache::pinFile(const folly::fbstring &uuid)
{
    LOG_FCALL() << LOG_FARG(uuid);

    assertInFiber();

    auto res = m_lruFileData.emplace(uuid, OpenFileData{});

    auto &lruData = res.first->second;

    if (!lruData.attr)
        lruData.attr = MetadataCache::getAttr(uuid);
    if (!lruData.location)
        lruData.location = MetadataCache::getLocation(uuid);

    ++lruData.openCount;

    LOG_DBG(2) << "Increased LRU open count of " << uuid << " to "
               << lruData.openCount;

    if (lruData.openCount > 1)
        m_onOpen(uuid);
}

std::shared_ptr<OpenFileMetadataCache::OpenFileToken>
OpenFileMetadataCache::open(const folly::fbstring &uuid)
{
    LOG_FCALL() << LOG_FARG(uuid);

    assertInFiber();

    try {
        FileAttrPtr attr;

        if (m_lruFileData.find(uuid) != m_lruFileData.end())
            attr = m_lruFileData.at(uuid).attr;
        else if (m_lruDirectoryData.find(uuid) != m_lruDirectoryData.end())
            attr = m_lruDirectoryData.at(uuid).attr;
        else
            attr = MetadataCache::getAttr(uuid);

        MetadataCache::ensureAttrAndLocationCached(uuid);

        pinFile(uuid);

        if (attr->parentUuid() && !attr->parentUuid().value().empty())
            noteDirectoryActivity(*attr->parentUuid());

        return std::make_shared<OpenFileToken>(std::move(attr), *this);
    }
    catch (...) {
        LOG(ERROR) << " Removing " << uuid
                   << " from LRU metadata cache due to unexpected error.";
        releaseFile(uuid);
        throw;
    }
}

std::shared_ptr<OpenFileMetadataCache::OpenFileToken>
OpenFileMetadataCache::open(const folly::fbstring &uuid,
    std::shared_ptr<FileAttr> attr, std::unique_ptr<FileLocation> location)
{
    LOG_FCALL() << LOG_FARG(uuid);

    assertInFiber();

    MetadataCache::updateAttr(attr);
    MetadataCache::putLocation(std::move(location));

    pinFile(uuid);

    if (attr->parentUuid() && !attr->parentUuid().value().empty())
        noteDirectoryActivity(*attr->parentUuid());

    return std::make_shared<OpenFileToken>(std::move(attr), *this);
}

void OpenFileMetadataCache::releaseFile(const folly::fbstring &uuid)
{
    LOG_FCALL() << LOG_FARG(uuid);

    assertInFiber();

    auto it = m_lruFileData.find(uuid);
    if (it == m_lruFileData.end())
        return;

    // If there are other file handles referring to this file
    // do nothing
    if (--it->second.openCount > 0)
        return;

    // Call on release handlers
    m_onRelease(uuid);

    if (it->second.lruIt)
        m_lruFileList.erase(it->second.lruIt.value());

    m_lruFileData.erase(it);

    MetadataCache::releaseFile(uuid);
}

FileAttrPtr OpenFileMetadataCache::getAttr(const folly::fbstring &uuid)
{
    LOG_FCALL() << LOG_FARG(uuid);

    assertInFiber();

    std::shared_ptr<FileAttr> attr;

    if (m_lruFileData.find(uuid) != m_lruFileData.end()) {
        attr = m_lruFileData.at(uuid).attr;
    }
    else if (m_lruDirectoryData.find(uuid) != m_lruDirectoryData.end()) {
        if (m_lruDirectoryData.at(uuid).deleted) {
            throw std::system_error(
                std::make_error_code(std::errc::no_such_file_or_directory));
        }

        attr = m_lruDirectoryData.at(uuid).attr;
    }
    else {
        attr = MetadataCache::getAttr(uuid);
    }

    assert(attr);

    if (attr->parentUuid() && !attr->parentUuid()->empty())
        noteDirectoryActivity(*attr->parentUuid());

    return attr;
}

FileAttrPtr OpenFileMetadataCache::getAttr(
    const folly::fbstring &parentUuid, const folly::fbstring &name)
{
    LOG_FCALL() << LOG_FARG(parentUuid) << LOG_FARG(name);

    assertInFiber();

    auto attr = MetadataCache::getAttr(parentUuid, name);

    if (attr->parentUuid() && !attr->parentUuid()->empty()) {
        noteDirectoryActivity(*attr->parentUuid());
    }

    return attr;
}

void OpenFileMetadataCache::putAttr(std::shared_ptr<FileAttr> attr)
{
    LOG_FCALL();

    assertInFiber();

    MetadataCache::putAttr(attr);
}

void OpenFileMetadataCache::noteDirectoryActivity(const folly::fbstring &uuid)
{
    LOG_FCALL() << LOG_FARG(uuid);

    assertInFiber();

    assert(!uuid.empty());

    auto res = m_lruDirectoryData.emplace(uuid, OpenFileData{});
    auto newEntry = res.second;
    auto &lruData = res.first->second;

    if (newEntry) {
        lruData.attr = MetadataCache::getAttr(uuid);
        lruData.lruIt =
            m_lruDirectoryList.emplace(m_lruDirectoryList.end(), uuid);
    }
    else if (lruData.lruIt) {
        // Move the entry to the end of the LRU list
        m_lruDirectoryList.splice(
            m_lruDirectoryList.end(), m_lruDirectoryList, *lruData.lruIt);
    }

    lruData.touch();
}

void OpenFileMetadataCache::pruneExpiredDirectories()
{
    LOG_FCALL();

    assertInFiber();

    // Invalidate all directories and their direct children which are
    // expired and do not contain any opened files
    // In case all cached files are opened, ensure loop limit using
    // maxIteraions
    auto maxIterations = m_lruDirectoryList.size();

    while (!m_lruDirectoryList.empty() && (maxIterations > 0)) {
        LOG_DBG(2) << "Directory LRU list size is: "
                   << m_lruDirectoryList.size();

        maxIterations--;

        auto &uuid = m_lruDirectoryList.front();
        auto oldestItem = m_lruDirectoryData.find(uuid);

        if (oldestItem == m_lruDirectoryData.end()) {
            // Directory no longer in cache - drop from LRU list
            m_lruDirectoryList.pop_front();
            continue;
        }

        if (oldestItem->second.expired(m_directoryCacheDropAfter) ||
            MetadataCache::size() > m_targetSize) {
            if (oldestItem->second.openCount > 0) {
                continue;
            }

            auto uuid = std::move(m_lruDirectoryList.front());
            m_lruDirectoryList.pop_front();
            m_lruDirectoryData.erase(uuid);
            m_onDropDirectory(uuid);

            // Invalidate all attributes from the directory
            MetadataCache::invalidateChildren(uuid);
        }
        else
            break;
    }
}

void OpenFileMetadataCache::prune()
{
    LOG_FCALL();

    assertInFiber();

    LOG_DBG(2) << "MetadataCache size is: " << MetadataCache::size()
               << " Maximum size is: " << m_targetSize;

    if (MetadataCache::size() > m_targetSize) {
        pruneExpiredDirectories();
    }
}

void OpenFileMetadataCache::clear()
{
    LOG_FCALL();

    assertInFiber();

    m_lruFileData.clear();
    m_lruFileList.clear();
    m_lruDirectoryList.clear();
    m_lruDirectoryData.clear();

    MetadataCache::clear();
}

void OpenFileMetadataCache::onReconnect()
{
    LOG_FCALL();

    assertInFiber();

    LOG_DBG(2) << "Updating opened files attrs "
                  "after reconnect";
    std::for_each(
        m_lruFileData.begin(), m_lruFileData.end(), [this](const auto &p) {
            LOG_DBG(2) << "Updating opened file attrs "
                          "after reconnect: "
                       << p.first;
            getAttr(p.first);
            getLocation(p.first, true);
            assert(getAttr(p.first)->size());
        });
}

bool OpenFileMetadataCache::rename(folly::fbstring uuid,
    folly::fbstring newParentUuid, folly::fbstring newName,
    folly::fbstring newUuid)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(newParentUuid)
                << LOG_FARG(newName) << LOG_FARG(newUuid);

    assertInFiber();

    assert(!newName.empty());

    if (m_lruDirectoryData.find(newParentUuid) != m_lruDirectoryData.end())
        noteDirectoryActivity(newParentUuid);

    // The client is not caching the parent of the old or new directory
    // and the file is not opened, in such case the rename event
    // can be ignored
    if (!MetadataCache::contains(uuid) &&
        m_lruFileData.find(uuid) == m_lruFileData.end() &&
        m_lruDirectoryData.find(newParentUuid) == m_lruDirectoryData.end())
        return false;

    // The client is caching the new directory to which the file was moved
    // but not the old directory, then we have to add the attribute to the
    // cache
    if (uuid != newUuid && m_lruFileData.find(uuid) == m_lruFileData.end() &&
        m_lruDirectoryData.find(newParentUuid) == m_lruDirectoryData.end()) {
        try {
            MetadataCache::markDeleted(uuid);
            MetadataCache::getAttr(newUuid);
        }
        catch (...) {
            return false;
        }

        return true;
    }

    // Recreate the subscriptions only if the old uuid is different from the new
    // one and the file is opened or directory is cached
    bool renewSubscriptions = (uuid != newUuid) &&
        (std::find(m_lruFileList.begin(), m_lruFileList.end(), uuid) !=
            m_lruFileList.end());

    return MetadataCache::rename(
        uuid, newParentUuid, newName, newUuid, renewSubscriptions);
}

void OpenFileMetadataCache::truncate(
    folly::fbstring uuid, const std::size_t newSize)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(newSize);

    assertInFiber();

    FileAttrPtr attr;

    if (m_lruFileData.find(uuid) != m_lruFileData.end())
        attr = m_lruFileData.at(uuid).attr;
    else if (m_lruDirectoryData.find(uuid) != m_lruDirectoryData.end())
        attr = m_lruDirectoryData.at(uuid).attr;
    else
        attr = MetadataCache::getAttr(uuid);

    if (attr->parentUuid() && !attr->parentUuid()->empty())
        noteDirectoryActivity(*attr->parentUuid());

    MetadataCache::truncate(uuid, newSize);
}

void OpenFileMetadataCache::updateTimes(
    folly::fbstring uuid, const messages::fuse::UpdateTimes &updateTimes)
{
    LOG_FCALL() << LOG_FARG(uuid);

    assertInFiber();

    FileAttrPtr attr;

    if (m_lruFileData.find(uuid) != m_lruFileData.end())
        attr = m_lruFileData.at(uuid).attr;
    else if (m_lruDirectoryData.find(uuid) != m_lruDirectoryData.end())
        attr = m_lruDirectoryData.at(uuid).attr;
    else
        attr = MetadataCache::getAttr(uuid);

    if (attr->parentUuid() && !attr->parentUuid()->empty())
        noteDirectoryActivity(*attr->parentUuid());

    MetadataCache::updateTimes(uuid, updateTimes);
}

void OpenFileMetadataCache::changeMode(
    const folly::fbstring &uuid, const mode_t newMode)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(newMode);

    assertInFiber();

    FileAttrPtr attr;

    if (m_lruFileData.find(uuid) != m_lruFileData.end())
        attr = m_lruFileData.at(uuid).attr;
    else if (m_lruDirectoryData.find(uuid) != m_lruDirectoryData.end())
        attr = m_lruDirectoryData.at(uuid).attr;
    else
        attr = MetadataCache::getAttr(uuid);

    if (attr->parentUuid() && !attr->parentUuid()->empty())
        noteDirectoryActivity(*attr->parentUuid());

    MetadataCache::changeMode(uuid, newMode);
}

void OpenFileMetadataCache::putLocation(std::unique_ptr<FileLocation> location)
{
    LOG_FCALL();

    assertInFiber();
    assert(location);

    auto uuid = location->uuid();

    try {
        MetadataCache::putLocation(std::move(location));

        if (m_lruFileData.find(uuid) != m_lruFileData.end())
            m_lruFileData.at(uuid).location = MetadataCache::getLocation(uuid);
    }
    catch (std::system_error &e) {
        if (e.code().value() == ENOENT) {
            LOG(WARNING) << "Cannot update location map - file '" << uuid
                         << "' alread deleted";
            return;
        }

        throw;
    }
}

std::shared_ptr<FileLocation> OpenFileMetadataCache::getLocation(
    const folly::fbstring &uuid, bool forceUpdate)
{
    LOG_FCALL();

    assertInFiber();

    // Check if the file is opened
    if ((m_lruFileData.find(uuid) != m_lruFileData.end())) {
        // If this request doesn't require updating the location,
        // just return the cached version
        if (!forceUpdate)
            return m_lruFileData.at(uuid).location;

        // If the file is deleted, or this is a forced update (e.g. after
        // read error) request the file location from the server
        if (MetadataCache::isDeleted(uuid) || forceUpdate) {
            m_lruFileData.find(uuid)->second.location =
                MetadataCache::getLocation(
                    m_lruFileData.find(uuid)->second.attr);

            return m_lruFileData.find(uuid)->second.location;
        }
    }

    // Return the cached location from metadata cache, or request one from
    // server if not available
    return MetadataCache::getLocation(uuid, forceUpdate);
}

bool OpenFileMetadataCache::updateLocation(const FileLocation &newLocation)
{
    LOG_FCALL();

    assertInFiber();

    auto const &uuid = newLocation.uuid();

    std::shared_ptr<FileLocation> location;

    if (m_lruFileData.find(uuid) != m_lruFileData.end()) {
        location = m_lruFileData.at(newLocation.uuid()).location;
    }
    else {
        try {
            location = MetadataCache::getLocation(newLocation.uuid());
        }
        catch (std::system_error &e) {
            if (e.code().value() != ENOENT)
                throw;
        }
    }

    if (!location)
        return false;

    location->version(newLocation.version());
    location->storageId(newLocation.storageId());
    location->fileId(newLocation.fileId());
    location->update(newLocation.blocks());

    LOG_DBG(2) << "Updated file location for file " << newLocation.uuid();

    return true;
}

bool OpenFileMetadataCache::updateLocation(
    const off_t start, const off_t end, const FileLocation &locationUpdate)
{
    LOG_FCALL() << LOG_FARG(start) << LOG_FARG(end)
                << LOG_FARG(locationUpdate.uuid());

    assertInFiber();

    auto const &uuid = locationUpdate.uuid();

    std::shared_ptr<FileLocation> location;

    if (m_lruFileData.find(uuid) != m_lruFileData.end()) {
        location = m_lruFileData.at(locationUpdate.uuid()).location;
    }
    else {
        try {
            location = MetadataCache::getLocation(locationUpdate.uuid());
        }
        catch (std::system_error &e) {
            if (e.code().value() != ENOENT)
                throw;
        }
    }

    if (!location)
        return false;

    location->version(locationUpdate.version());
    location->storageId(locationUpdate.storageId());
    location->fileId(locationUpdate.fileId());
    location->updateInRange(start, end, locationUpdate);

    LOG_DBG(2) << "Updated file location for file " << locationUpdate.uuid()
               << " in range [" << start << ", " << end << ")";

    return true;
}

void OpenFileMetadataCache::addBlock(const folly::fbstring &uuid,
    const boost::icl::discrete_interval<off_t> range,
    messages::fuse::FileBlock fileBlock)
{
    LOG_FCALL() << LOG_FARG(uuid);

    assertInFiber();

    std::shared_ptr<FileLocation> location;

    if (m_lruFileData.find(uuid) != m_lruFileData.end() &&
        m_lruFileData[uuid].deleted) {
        // If file is opened and deleted, update only the temporarily cached
        // attributed, as the file is already removed in the metadata cache
        auto newSize = std::max<off_t>(
            boost::icl::last(range) + 1, *m_lruFileData.at(uuid).attr->size());
        m_lruFileData.at(uuid).attr->size(newSize);
        location = m_lruFileData.at(uuid).location;
    }
    else {
        // Update the attribute in the general cache
        MetadataCache::ensureAttrAndLocationCached(uuid);
        MetadataCache::updateSizeFromRange(uuid, range);

        // Ensure location is updated
        try {
            location = MetadataCache::getLocation(uuid);
        }
        catch (std::system_error &e) {
            if (e.code().value() == ENOENT) {
                return;
            }
        }
    }

    assert(location);

    auto newBlock = std::make_pair(range, std::move(fileBlock));
    location->putBlock(newBlock);
}

folly::Optional<
    std::pair<boost::icl::discrete_interval<off_t>, messages::fuse::FileBlock>>
OpenFileMetadataCache::getBlock(const folly::fbstring &uuid, const off_t offset)
{
    assertInFiber();

    FileAttrPtr attr;
    std::shared_ptr<FileLocation> location;

    if (m_lruFileData.find(uuid) != m_lruFileData.end()) {
        // Check if the uuid points to an opened file
        attr = m_lruFileData.at(uuid).attr;
        location = m_lruFileData.at(uuid).location;
    }
    else {
        // Get the attribute from the general cache
        try {
            attr = MetadataCache::getAttr(uuid);
            location = MetadataCache::getLocation(uuid);
        }
        catch (std::system_error &e) {
            if (e.code().value() == ENOENT) {
                return {};
            }
        }
    }

    assert(location);
    assert(attr);

    auto availableBlockIt =
        location->blocks().find(boost::icl::discrete_interval<off_t>(offset));

    if (availableBlockIt != location->blocks().end())
        return std::make_pair(
            availableBlockIt->first, availableBlockIt->second);

    return {};
}

messages::fuse::FileBlock OpenFileMetadataCache::getDefaultBlock(
    const folly::fbstring &uuid)
{
    assertInFiber();

    std::shared_ptr<FileAttr> attr;
    std::shared_ptr<FileLocation> location;

    if (m_lruFileData.find(uuid) != m_lruFileData.end()) {
        // Check if the uuid points to an opened file
        attr = m_lruFileData.at(uuid).attr;
        location = m_lruFileData.at(uuid).location;
    }
    else {
        // Get the attribute from the general cache
        try {
            attr = MetadataCache::getAttr(uuid);
            location = MetadataCache::getLocation(uuid);
        }
        catch (std::system_error &e) {
            if (e.code().value() == ENOENT) {
                return {};
            }
        }
    }

    assert(location);
    assert(attr);

    return messages::fuse::FileBlock{location->storageId(), location->fileId()};
}

const std::string &OpenFileMetadataCache::getSpaceId(
    const folly::fbstring &uuid)
{
    assertInFiber();

    std::shared_ptr<FileLocation> location;

    if (m_lruFileData.find(uuid) != m_lruFileData.end()) {
        // Check if the uuid points to an opened file
        location = m_lruFileData.at(uuid).location;
    }
    else {
        // Get the attribute from the general cache
        location = MetadataCache::getLocation(uuid);
    }

    assert(location);

    return location->spaceId();
}

bool OpenFileMetadataCache::updateAttr(std::shared_ptr<FileAttr> newAttr)
{
    assertInFiber();

    try {
        if (MetadataCache::updateAttr(newAttr))
            return true;
    }
    catch (std::system_error &e) {
        if (e.code().value() == ENOENT) {
            return false;
        }
    }

    // Check if the uuid points to an opened file
    if (m_lruFileData.find(newAttr->uuid()) != m_lruFileData.end()) {
        auto attr = m_lruFileData.at(newAttr->uuid()).attr;
        auto location = m_lruFileData.at(newAttr->uuid()).location;
        if (attr->type() == FileAttr::FileType::regular) {
            if (newAttr->size() && attr->size() &&
                (*newAttr->size() < *attr->size()) && location) {
                LOG_DBG(2)
                    << "Truncating file size based on updated attributes "
                       "for uuid: '"
                    << newAttr->uuid() << "'";

                location->truncate(
                    boost::icl::discrete_interval<off_t>::right_open(
                        0, *newAttr->size()));
            }
            if (newAttr->size())
                attr->size(*newAttr->size());
        }

        attr->atime(std::max(attr->atime(), newAttr->atime()));
        attr->ctime(std::max(attr->ctime(), newAttr->ctime()));
        attr->mtime(std::max(attr->mtime(), newAttr->mtime()));

        attr->gid(newAttr->gid());
        attr->mode(newAttr->mode());
        attr->uid(newAttr->uid());
    }

    return false;
}

void OpenFileMetadataCache::handleMarkDeleted(const folly::fbstring &uuid)
{
    LOG_FCALL() << LOG_FARG(uuid);

    assertInFiber();
    assert(!uuid.empty());

    // Try to treat the uuid as directory
    auto itd = m_lruDirectoryData.find(uuid);
    if (itd != m_lruDirectoryData.end()) {
        if (itd->second.deleted)
            LOG(WARNING) << "Deleting already deleted directory: " << uuid;

        itd->second.deleted = true;

        if (itd->second.openCount == 0) {
            if (itd->second.lruIt)
                m_lruDirectoryList.erase(*itd->second.lruIt);

            m_lruDirectoryData.erase(uuid);
        }
    }
    else {
        auto itf = m_lruFileData.find(uuid);
        if (itf == m_lruFileData.end())
            return;

        if (itf->second.deleted)
            LOG(WARNING) << "Deleting already deleted file: " << uuid;

        itf->second.deleted = true;
    }

    m_onMarkDeleted(uuid);
}

void OpenFileMetadataCache::handleRename(
    const folly::fbstring &oldUuid, const folly::fbstring &newUuid)
{
    LOG_FCALL() << LOG_FARG(oldUuid) << LOG_FARG(newUuid);

    assertInFiber();
    assert(!newUuid.empty());

    auto attr = getAttr(newUuid);

    if (attr->type() == FileAttr::FileType::directory) {
        // Handle rename of a cached directory
        auto it = m_lruDirectoryData.find(oldUuid);
        if (it == m_lruDirectoryData.end())
            return;

        auto lruData = std::move(it->second);
        m_lruDirectoryData.erase(it);
        auto res = m_lruDirectoryData.emplace(newUuid, OpenFileData{});
        if (res.second) {
            res.first->second = std::move(lruData);
            if (res.first->second.lruIt) {
                auto oldIt = *(res.first->second.lruIt);
                res.first->second.lruIt =
                    m_lruDirectoryList.emplace(oldIt, newUuid);
                m_lruDirectoryList.erase(oldIt);
            }
        }
        else {
            LOG(WARNING) << "Target UUID '" << newUuid
                         << "' of rename is already used; merging metadata "
                            "usage records.";

            auto &oldRecord = res.first->second;
            oldRecord.openCount += lruData.openCount;
            oldRecord.deleted = oldRecord.deleted || lruData.deleted;

            if (lruData.lruIt)
                m_lruDirectoryList.erase(*lruData.lruIt);
        }
    }
    else {
        //
        // Handle rename of opened file
        //
        auto it = m_lruFileData.find(oldUuid);
        if (it == m_lruFileData.end())
            return;

        auto lruData = std::move(it->second);
        m_lruFileData.erase(it);

        auto res = m_lruFileData.emplace(newUuid, OpenFileData{});
        if (res.second) {
            res.first->second = std::move(lruData);
            if (res.first->second.lruIt) {
                auto oldIt = *(res.first->second.lruIt);
                res.first->second.lruIt = m_lruFileList.emplace(oldIt, newUuid);
                m_lruFileList.erase(oldIt);
            }
        }
        else {
            LOG(WARNING) << "Target UUID '" << newUuid
                         << "' of rename is already used; merging metadata "
                            "usage records.";

            auto &oldRecord = res.first->second;
            oldRecord.openCount += lruData.openCount;
            oldRecord.deleted = oldRecord.deleted || lruData.deleted;

            if (lruData.lruIt)
                m_lruFileList.erase(*lruData.lruIt);
        }
    }

    m_onRename(oldUuid, newUuid);
}

} // namespace cache
} // namespace client
} // namespace one
