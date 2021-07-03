/**
 * @file helpersCache.cc
 * @author Konrad Zemek
 * @copyright (C) 2015 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#include "helpersCache.h"

#include "buffering/bufferAgent.h"
#include "messages.pb.h"
#include "messages/fuse/createStorageTestFile.h"
#include "messages/fuse/getHelperParams.h"
#include "messages/fuse/helperParams.h"
#include "messages/fuse/storageTestFile.h"
#include "messages/fuse/verifyStorageTestFile.h"

#include <folly/ThreadName.h>

#include <algorithm>
#include <chrono>
#include <functional>

namespace one {
namespace client {
namespace cache {

HelpersCache::HelpersCache(communication::Communicator &communicator,
    std::shared_ptr<Scheduler> scheduler, const options::Options &options)
    : m_communicator{communicator}
    , m_scheduler{std::move(scheduler)}
    , m_options{options}
    , m_helpersIOExecutor{std::make_shared<folly::IOThreadPoolExecutor>(
          static_cast<int>(options.getStorageHelperThreadCount()))}
    , m_helperParamOverrides{options.getHelperOverrideParams()}
    , m_helperFactory
{
#if WITH_CEPH
    m_helpersIOExecutor, m_helpersIOExecutor,
#endif
        m_helpersIOExecutor,
#if WITH_S3
        m_helpersIOExecutor,
#endif
#if WITH_SWIFT
        m_helpersIOExecutor,
#endif
#if WITH_GLUSTERFS
        m_helpersIOExecutor,
#endif
#if WITH_WEBDAV
        m_helpersIOExecutor,
#endif
#if WITH_XROOTD
        m_helpersIOExecutor,
#endif
        m_helpersIOExecutor, m_communicator,
        options.getBufferSchedulerThreadCount(),
        helpers::buffering::BufferLimits{options.getReadBufferMinSize(),
            options.getReadBufferMaxSize(),
            options.getReadBufferPrefetchDuration(),
            options.getWriteBufferMinSize(), options.getWriteBufferMaxSize(),
            options.getWriteBufferFlushDelay(),
            options::DEFAULT_PREFETCH_TARGET_LATENCY,
            options::DEFAULT_PREFETCH_POWER_BASE,
            options.getReadBuffersTotalSize(),
            options.getWriteBuffersTotalSize()},
        helpers::ExecutionContext::ONECLIENT
}
, m_storageAccessManager{m_helperFactory, m_options},
    m_providerTimeout{options.getProviderTimeout()}
{
}

HelpersCache::~HelpersCache() {}

HelpersCache::AccessType HelpersCache::getAccessType(
    const folly::fbstring &storageId)
{
    std::lock_guard<std::mutex> guard(m_accessTypeMutex);

    if (m_accessType.find(storageId) == m_accessType.end())
        return AccessType::UNKNOWN;

    return m_accessType[storageId];
}

folly::Future<folly::Unit> HelpersCache::refreshHelperParameters(
    const folly::fbstring &storageId, const folly::fbstring &spaceId)
{
    LOG_FCALL() << LOG_FARG(storageId) << LOG_FARG(spaceId);

    std::lock_guard<std::mutex> guard(m_cacheMutex);

    // Get the helper promise if exists already
    auto helperKey = std::make_pair(storageId, false);
    auto helperPromiseIt = m_cache.find(helperKey);

    if (helperPromiseIt == m_cache.end()) {
        LOG(WARNING) << "Trying to refresh parameters for nonexisting helper "
                        "to storage: "
                     << storageId;
        return folly::makeFuture();
    }

    // Invalidate helper parameters and obtain a new parameters promise
    return helperPromiseIt->second->getFuture().then([this, storageId, spaceId](
                                                         HelpersCache::HelperPtr
                                                             helper) {
        auto params = communication::wait(
            m_communicator.communicate<messages::fuse::HelperParams>(
                messages::fuse::GetHelperParams{storageId.toStdString(),
                    spaceId.toStdString(),
                    messages::fuse::GetHelperParams::HelperMode::directMode}),
            m_providerTimeout);

        auto helperParams =
            helpers::StorageHelperParams::create(params.name(), params.args());

        auto bufferedHelper =
            std::dynamic_pointer_cast<helpers::buffering::BufferAgent>(helper);
        if (bufferedHelper)
            return bufferedHelper->helper()->refreshParams(
                std::move(helperParams));

        return helper->refreshParams(std::move(helperParams));
    });
}

folly::Future<HelpersCache::HelperPtr> HelpersCache::get(
    const folly::fbstring &fileUuid, const folly::fbstring &spaceId,
    const folly::fbstring &storageId, bool forceProxyIO, bool proxyFallback)
{
    LOG_FCALL() << LOG_FARG(fileUuid) << LOG_FARG(storageId)
                << LOG_FARG(forceProxyIO);

    LOG_DBG(2) << "Getting storage helper for file " << fileUuid
               << " on storage " << storageId;

    if (!proxyFallback) {
        if (m_options.isDirectIOForced() && forceProxyIO) {
            LOG(ERROR) << "Direct IO and force IO options cannot be "
                          "simultanously set.";
            throw std::errc::operation_not_supported; // NOLINT
        }

        if (m_options.isDirectIOForced()) {
            auto helperKey = std::make_pair(storageId, false);
            auto helperPromiseIt = m_cache.find(helperKey);

            std::lock_guard<std::mutex> guard(m_cacheMutex);

            if (helperPromiseIt == m_cache.end()) {
                LOG_DBG(2)
                    << "Storage helper promise for storage " << storageId
                    << " in direct mode unavailable - creating new storage "
                       "helper...";

                auto p = std::make_shared<folly::SharedPromise<HelperPtr>>();

                m_cache.emplace(std::make_tuple(storageId, false), p);

                m_scheduler->post(
                    [this, &fileUuid, &spaceId, &storageId, p = std::move(p)] {
                        p->setWith([=] {
                            return performForcedDirectIOStorageDetection(
                                fileUuid, spaceId, storageId);
                        });
                    });
            }

            return m_cache.find(helperKey)->second->getFuture();
        }
    }

    forceProxyIO |= (m_options.isProxyIOForced() || proxyFallback);

    auto helperKey = std::make_pair(storageId, forceProxyIO);

    std::lock_guard<std::mutex> guard(m_cacheMutex);

    auto helperPromiseIt = m_cache.find(helperKey);
    if (helperPromiseIt == m_cache.end()) {
        LOG_DBG(2) << "Storage helper promise for storage " << storageId
                   << " unavailable - creating new storage helper...";

        auto p = std::make_shared<folly::SharedPromise<HelperPtr>>();

        m_cache.emplace(std::make_tuple(storageId, forceProxyIO), p);

        m_scheduler->post([this, &fileUuid, &spaceId, &storageId, forceProxyIO,
                              p = std::move(p)] {
            p->setWith([=] {
                return performAutoIOStorageDetection(
                    fileUuid, spaceId, storageId, forceProxyIO);
            });
        });
    }

    return m_cache.find(helperKey)->second->getFuture();
}

HelpersCache::HelperPtr HelpersCache::performAutoIOStorageDetection(
    const folly::fbstring &fileUuid, const folly::fbstring &spaceId,
    const folly::fbstring &storageId, bool forceProxyIO)
{
    LOG(INFO)
        << "Performing automatic storage access type detection for storage "
        << storageId << " for file " << fileUuid
        << " with forced proxy io mode: " << forceProxyIO;

    bool accessUnset;
    auto accessTypeKey = std::make_pair(storageId, AccessType::PROXY);

    // Check if the access type (PROXY or DIRECT) is already
    // determined for storage 'storageId'
    {
        std::lock_guard<std::mutex> guard(m_accessTypeMutex);
        std::tie(std::ignore, accessUnset) =
            m_accessType.emplace(accessTypeKey);
    }

    if (!forceProxyIO) {
        if (accessUnset) {
            std::unordered_map<folly::fbstring, folly::fbstring> overrideParams;
            if (m_helperParamOverrides.find(storageId) !=
                m_helperParamOverrides.end())
                overrideParams = m_helperParamOverrides.at(storageId);

            auto params = communication::wait(
                m_communicator.communicate<messages::fuse::HelperParams>(
                    messages::fuse::GetHelperParams{storageId.toStdString(),
                        spaceId.toStdString(),
                        messages::fuse::GetHelperParams::HelperMode::
                            directMode}),
                m_providerTimeout);

            if (params.name() == helpers::PROXY_HELPER_NAME) {
                LOG(INFO) << "Storage " << storageId
                          << " not accessible for direct access from this "
                             "Oneprovider - switching to proxy mode.";

                return m_helperFactory.getStorageHelper(params.name(),
                    params.args(), m_options.isIOBuffered(), overrideParams);
            }

            if (params.name() == helpers::POSIX_HELPER_NAME &&
                overrideParams.find("mountPoint") != overrideParams.end()) {

                m_storageAccessManager.checkPosixMountpointOverride(
                    storageId, overrideParams);

                {
                    std::lock_guard<std::mutex> guard(m_accessTypeMutex);
                    auto at = m_accessType.emplace(
                        std::make_pair(storageId, AccessType::DIRECT));
                    if (!at.second)
                        at.first->second = AccessType::DIRECT;
                }

                return m_helperFactory.getStorageHelper(params.name(),
                    params.args(), m_options.isIOBuffered(), overrideParams);
            }

            // First try to quickly detect direct io (in 1 attempt), if not
            // available, return proxy and schedule full storage detection
            auto helper =
                requestStorageTestFileCreation(fileUuid, storageId, 1);
            if (helper) {
                LOG(INFO) << "Direct access to " << params.name() << " storage "
                          << storageId << " determined on first attempt";
                return helper;
            }

            LOG_DBG(2) << "Direct access to storage " << storageId
                       << " wasn't determined on first attempt - "
                          "scheduling retry and return proxy helper as "
                          "fallback";
            m_scheduler->post([this, fileUuid, storageId,
                                  storageType = params.name()] {
                auto directIOHelper =
                    requestStorageTestFileCreation(fileUuid, storageId);
                if (directIOHelper) {
                    LOG_DBG(2) << "Found direct access to " << storageType
                               << " storage " << storageId
                               << " using automatic storage detection";
                    {
                        std::lock_guard<std::mutex> guard(m_cacheMutex);
                        m_cache[std::make_tuple(storageId, false)]->setValue(
                            directIOHelper);
                    }

                    {
                        std::lock_guard<std::mutex> guard(m_accessTypeMutex);
                        auto at = m_accessType.emplace(
                            std::make_pair(storageId, AccessType::DIRECT));
                        if (!at.second)
                            at.first->second = AccessType::DIRECT;
                    }
                }
                else {
                    LOG(INFO) << "Direct access to " << storageType
                              << " storage " << storageId
                              << " couldn't be established - leaving "
                                 "proxy access";
                }
            });
            return performAutoIOStorageDetection(
                fileUuid, spaceId, storageId, true);
        }

        std::lock_guard<std::mutex> guard(m_accessTypeMutex);
        if (m_accessType[storageId] == AccessType::PROXY)
            return performAutoIOStorageDetection(
                fileUuid, spaceId, storageId, true);
    }

    auto params = communication::wait(
        m_communicator.communicate<messages::fuse::HelperParams>(
            messages::fuse::GetHelperParams{storageId.toStdString(),
                spaceId.toStdString(),
                messages::fuse::GetHelperParams::HelperMode::proxyMode}),
        m_providerTimeout);

    std::unordered_map<folly::fbstring, folly::fbstring> overrideParams;
    if (m_helperParamOverrides.find(storageId) != m_helperParamOverrides.end())
        overrideParams = m_helperParamOverrides.at(storageId);

    return m_helperFactory.getStorageHelper(
        params.name(), params.args(), m_options.isIOBuffered(), overrideParams);
}

HelpersCache::HelperPtr HelpersCache::performForcedDirectIOStorageDetection(
    const folly::fbstring &fileUuid, const folly::fbstring &spaceId,
    const folly::fbstring &storageId)
{
    LOG_DBG(1) << "Requesting helper parameters for storage " << storageId
               << " in forced direct IO mode";

    {
        std::lock_guard<std::mutex> guard(m_accessTypeMutex);
        m_accessType.emplace(std::make_pair(storageId, AccessType::DIRECT));
    }

    try {
        std::unordered_map<folly::fbstring, folly::fbstring> overrideParams;
        if (m_helperParamOverrides.find(storageId) !=
            m_helperParamOverrides.end())
            overrideParams = m_helperParamOverrides.at(storageId);

        auto params = communication::wait(
            m_communicator.communicate<messages::fuse::HelperParams>(
                messages::fuse::GetHelperParams{storageId.toStdString(),
                    spaceId.toStdString(),
                    messages::fuse::GetHelperParams::HelperMode::directMode}),
            m_providerTimeout);

        if (params.name() == helpers::PROXY_HELPER_NAME) {
            LOG(ERROR) << "File " << fileUuid
                       << " is not accessible in direct IO mode "
                          "on this provider";
            throw std::errc::operation_not_supported; // NOLINT
        }

        if (params.name() == helpers::POSIX_HELPER_NAME &&
            overrideParams.find("mountPoint") == overrideParams.end()) {
            LOG(INFO) << "Direct IO requested to Posix storage " << storageId
                      << " - "
                         "attempting storage mountpoint detection in local "
                         "filesystem";

            return requestStorageTestFileCreation(fileUuid, storageId);
        }

        LOG_DBG(1) << "Got storage helper params for file " << fileUuid
                   << " on " << params.name() << " storage " << storageId;

        m_storageAccessManager.checkPosixMountpointOverride(
            storageId, overrideParams);

        return m_helperFactory.getStorageHelper(params.name(), params.args(),
            m_options.isIOBuffered(), overrideParams);
    }
    catch (std::exception &e) {
        LOG_DBG(1) << "Unexpected error when waiting for "
                      "storage helper: "
                   << e.what();
        throw std::errc::resource_unavailable_try_again; // NOLINT
    }
}

HelpersCache::HelperPtr HelpersCache::requestStorageTestFileCreation(
    const folly::fbstring &fileUuid, const folly::fbstring &storageId,
    const int maxAttempts)
{
    LOG_DBG(1) << "Requesting storage test file creation for file: '"
               << fileUuid << "' and storage: '" << storageId << "'";

    try {
        auto testFile = communication::wait(
            m_communicator.communicate<messages::fuse::StorageTestFile>(
                messages::fuse::CreateStorageTestFile{
                    fileUuid.toStdString(), storageId.toStdString()}),
            m_providerTimeout);

        auto sharedTestFileMsg =
            std::make_shared<messages::fuse::StorageTestFile>(
                std::move(testFile));

        return handleStorageTestFile(sharedTestFileMsg, storageId, maxAttempts);
    }
    catch (const std::system_error &e) {
        LOG(WARNING) << "Storage test file creation error, code: '" << e.code()
                     << "', message: '" << e.what() << "'";

        if (e.code().value() == EAGAIN) {
            std::lock_guard<std::mutex> guard(m_accessTypeMutex);
            m_accessType.erase(storageId);
        }
        else
            LOG(INFO) << "Storage '" << storageId
                      << "' is not directly accessible to the client.";

        return {};
    }
}

HelpersCache::HelperPtr HelpersCache::handleStorageTestFile(
    std::shared_ptr<messages::fuse::StorageTestFile> testFile,
    const folly::fbstring &storageId, const int maxAttempts)
{
    LOG_DBG(1) << "Handling storage test file for storage: " << storageId;

    try {
        auto helper =
            m_storageAccessManager.verifyStorageTestFile(storageId, *testFile);
        auto attempts = maxAttempts;

        while (!helper && (attempts-- > 0)) {
            std::this_thread::sleep_for(VERIFY_TEST_FILE_DELAY);
            helper = m_storageAccessManager.verifyStorageTestFile(
                storageId, *testFile);
        }

        if (!helper) {
            LOG(INFO) << "Storage '" << storageId
                      << "' is not directly accessible to the client. Test "
                         "file verification attempts limit ("
                      << maxAttempts << ") exceeded.";

            std::lock_guard<std::mutex> guard(m_accessTypeMutex);
            m_accessType[storageId] = AccessType::PROXY;
            return {};
        }

        auto fileContent = m_storageAccessManager.modifyStorageTestFile(
            storageId, helper, *testFile);

        requestStorageTestFileVerification(*testFile, storageId, fileContent);

        return helper;
    }
    catch (const std::system_error &e) {
        LOG(ERROR) << "Storage test file handling error, code: '" << e.code()
                   << "', message: '" << e.what() << "'";

        std::lock_guard<std::mutex> guard(m_accessTypeMutex);

        if (e.code().value() == EAGAIN) {
            m_accessType.erase(storageId);
        }
        else {
            LOG(INFO) << "Storage '" << storageId
                      << "' is not directly accessible to the client.";

            m_accessType[storageId] = AccessType::PROXY;
        }

        return {};
    }
}

void HelpersCache::requestStorageTestFileVerification(
    const messages::fuse::StorageTestFile &testFile,
    const folly::fbstring &storageId, const folly::fbstring &fileContent)
{
    LOG(INFO) << "Requesting verification of storage: '" << storageId
              << "' of type '" << testFile.helperParams().name();

    if (testFile.helperParams().name() == helpers::NULL_DEVICE_HELPER_NAME) {
        handleStorageTestFileVerification({}, storageId);
        return;
    }

    messages::fuse::VerifyStorageTestFile request{storageId.toStdString(),
        testFile.spaceId(), testFile.fileId(), fileContent.toStdString()};

    try {
        communication::wait(
            m_communicator.communicate<messages::fuse::FuseResponse>(
                std::move(request)),
            m_providerTimeout);

        handleStorageTestFileVerification({}, storageId);
    }
    catch (const std::system_error &e) {
        handleStorageTestFileVerification(e.code(), {});
    }
}

void HelpersCache::handleStorageTestFileVerification(
    const std::error_code &ec, const folly::fbstring &storageId)
{
    LOG_DBG(1) << "Handling verification of storage direct access: "
               << storageId;

    std::lock_guard<std::mutex> guard(m_accessTypeMutex);

    if (!ec) {
        LOG(INFO) << "Storage " << storageId
                  << " is directly accessible to the client.";

        m_accessType[storageId] = AccessType::DIRECT;
    }
    else {
        LOG(ERROR) << "Storage test file verification error, code: '"
                   << ec.value() << "', message: '" << ec.message() << "'";

        if (ec.value() == EAGAIN) {
            m_accessType.erase(storageId);
        }
        else {
            LOG(INFO) << "Storage '" << storageId
                      << "' is not directly accessible to the client.";

            m_accessType[storageId] = AccessType::PROXY;
        }
    }
}

} // namespace cache
} // namespace client
} // namespace one
