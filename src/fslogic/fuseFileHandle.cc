/**
 * @file fuseFileHandle.cc
 * @author Konrad Zemek
 * @copyright (C) 2016 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#include "fuseFileHandle.h"

#include "cache/forceProxyIOCache.h"
#include "cache/helpersCache.h"
#include "helpers/logging.h"

namespace one {
namespace client {
namespace fslogic {

constexpr auto FSLOGIC_RECENT_PREFETCH_CACHE_SIZE = 1000U;
constexpr auto FSLOGIC_RECENT_PREFETCH_CACHE_PRUNE_SIZE = 50U;

FuseFileHandle::FuseFileHandle(const int flags_, folly::fbstring handleId,
    std::shared_ptr<cache::OpenFileMetadataCache::OpenFileToken> openFileToken,
    cache::HelpersCacheBase &helpersCache,
    cache::ForceProxyIOCache &forceProxyIOCache,
    const std::chrono::seconds providerTimeout,
    const unsigned int prefetchCalculateSkipReads,
    const unsigned int prefetchCalculateAfterSeconds)
    : m_flags{flags_}
    , m_handleId{std::move(handleId)}
    , m_openFileToken{std::move(openFileToken)}
    , m_helpersCache{helpersCache}
    , m_forceProxyIOCache{forceProxyIOCache}
    , m_providerTimeout{providerTimeout}
    , m_fullPrefetchTriggered{false}
    , m_tagOnCreateSet{false}
    , m_tagOnModifySet{false}
    , m_recentPrefetchOffsets{folly::EvictingCacheMap<off_t, bool>(
          FSLOGIC_RECENT_PREFETCH_CACHE_SIZE,
          FSLOGIC_RECENT_PREFETCH_CACHE_PRUNE_SIZE)}
    , m_prefetchCalculateSkipReads{prefetchCalculateSkipReads}
    , m_prefetchCalculateAfterSeconds{prefetchCalculateAfterSeconds}
    , m_readsSinceLastPrefetchCalculation{0}
    , m_timeOfLastPrefetchCalculation{std::chrono::system_clock::now()}
{
}

folly::Future<helpers::FileHandlePtr> FuseFileHandle::getHelperHandle(
    const folly::fbstring &uuid, const folly::fbstring &spaceId,
    const folly::fbstring &storageId, const folly::fbstring &fileId)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(storageId) << LOG_FARG(fileId);

    const bool forceProxyIO = m_forceProxyIOCache.contains(uuid);
    const auto key = std::make_tuple(storageId, fileId, forceProxyIO);
    bool proxyFallback = false;
    if (forceProxyIO)
        proxyFallback = m_forceProxyIOCache.get(uuid);

    auto it = m_helperHandles.find(key);
    if (it != m_helperHandles.end())
        return it->second;

    auto helper =
        m_helpersCache
            .get(uuid, spaceId, storageId, forceProxyIO, proxyFallback)
            .get();

    if (!helper) {
        LOG(ERROR) << "Could not create storage helper for file " << uuid
                   << " on storage " << storageId;
        throw std::errc::resource_unavailable_try_again; // NOLINT
    }

    const auto filteredFlags = m_flags & (~O_CREAT) & (~O_APPEND);

    return // auto handle = // communication::wait(
        helper->open(fileId, filteredFlags, makeParameters(uuid))
            .thenTry([this, key](auto &&maybeHandle) {
                maybeHandle.throwIfFailed();
                assert(maybeHandle.value());
                m_helperHandles[key] = maybeHandle.value();
                return maybeHandle.value();
            });

    // return handle;
}

void FuseFileHandle::releaseHelperHandle(const folly::fbstring &uuid,
    const folly::fbstring &storageId, const folly::fbstring &fileId)
{
    LOG_FCALL() << LOG_FARG(uuid) << LOG_FARG(storageId) << LOG_FARG(fileId);

    for (bool forceProxyIO : {true, false}) {
        const auto key = std::make_tuple(storageId, fileId, forceProxyIO);
        auto it = m_helperHandles.find(key);
        if (it != m_helperHandles.end()) {
            communication::wait(it->second->release(), m_providerTimeout);
            m_helperHandles.erase(key);
        }
    }
}

void FuseFileHandle::reset()
{
    LOG_FCALL();

    m_helperHandles.clear();
}

folly::fbvector<helpers::FileHandlePtr> FuseFileHandle::helperHandles() const
{
    folly::fbvector<helpers::FileHandlePtr> result;
    for (const auto &elem : m_helperHandles)
        result.emplace_back(elem.second);
    return result;
}

helpers::FileHandlePtr FuseFileHandle::helperHandle(
    const folly::fbstring &storageId) const
{
    for (const auto &elem : m_helperHandles) {
        if ((std::get<0>(elem.first) == storageId) &&
            (!std::get<2>(elem.first)))
            return elem.second;
    }

    return {};
}

folly::Optional<folly::fbstring> FuseFileHandle::providerHandleId() const
{
    return m_handleId;
}

std::unordered_map<folly::fbstring, folly::fbstring>
FuseFileHandle::makeParameters(const folly::fbstring &uuid)
{
    std::unordered_map<folly::fbstring, folly::fbstring> parameters{
        {"file_uuid", uuid}, {"handle_id", m_handleId}};

    return parameters;
};

bool FuseFileHandle::prefetchAlreadyRequestedAt(off_t offset) const
{
    return m_recentPrefetchOffsets.withRLock(
        [&](const auto &cache) { return cache.exists(offset); });
}

void FuseFileHandle::addPrefetchAt(off_t offset)
{
    return m_recentPrefetchOffsets.withWLock(
        [&](auto &cache) { return cache.set(offset, true); });
}

bool FuseFileHandle::shouldCalculatePrefetch()
{
    if (m_readsSinceLastPrefetchCalculation > m_prefetchCalculateSkipReads ||
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now() - m_timeOfLastPrefetchCalculation)
                .count() > m_prefetchCalculateAfterSeconds) {
        m_readsSinceLastPrefetchCalculation = 0;
        m_timeOfLastPrefetchCalculation = std::chrono::system_clock::now();
        return true;
    }

    m_readsSinceLastPrefetchCalculation++;
    return false;
}

} // namespace fslogic
} // namespace client
} // namespace one
