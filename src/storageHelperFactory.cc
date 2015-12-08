/**
 * @file storageHelperFactory.cc
 * @author Rafal Slota
 * @copyright (C) 2013 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#include "helpers/storageHelperFactory.h"

#include "directIOHelper.h"

namespace one {
namespace helpers {

StorageHelperFactory::StorageHelperFactory(asio::io_service &dio_service)
    : m_dioService{dio_service}
{
}

std::shared_ptr<IStorageHelper> StorageHelperFactory::getStorageHelper(
    const std::string &sh_name,
    const std::unordered_map<std::string, std::string> &args)
{
    if (sh_name == "DirectIO")
#ifdef __linux__
        return std::make_shared<DirectIOHelper>(args, m_dioService,
                                                      DirectIOHelper::linux_user_ctx_factory);
#else
        return std::make_shared<DirectIOHelper>(args, m_dioService,
                                                      DirectIOHelper::noop_user_ctx_factory);
#endif

    return {};
}

} // namespace helpers
} // namespace one
