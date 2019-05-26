/**
 * @file getHelperParams.cc
 * @author Konrad Zemek
 * @copyright (C) 2015 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#include "getHelperParams.h"

#include "messages.pb.h"

#include <memory>
#include <sstream>

namespace one {
namespace messages {
namespace fuse {

GetHelperParams::GetHelperParams(
    std::string storageId, std::string spaceId, HelperMode mode)
    : m_storageId{std::move(storageId)}
    , m_spaceId{std::move(spaceId)}
    , m_mode{mode}
{
}

std::string GetHelperParams::toString() const
{
    std::stringstream stream;

    stream << "type: 'GetHelperParams', storageId: '" << m_storageId
           << "', spaceId: '" << m_spaceId << "', mode: ";

    switch (m_mode) {
        case HelperMode::autoMode:
            stream << "AUTO";
            break;
        case HelperMode::directMode:
            stream << "FORCE_DIRECT";
            break;
        case HelperMode::proxyMode:
            stream << "FORCE_PROXY";
    }

    return stream.str();
}

std::unique_ptr<ProtocolClientMessage> GetHelperParams::serializeAndDestroy()
{
    auto msg = std::make_unique<ProtocolClientMessage>();
    auto ghp = msg->mutable_fuse_request()->mutable_get_helper_params();

    ghp->mutable_storage_id()->swap(m_storageId);
    ghp->mutable_space_id()->swap(m_spaceId);

    switch (m_mode) {
        case HelperMode::autoMode:
            ghp->set_helper_mode(one::clproto::HelperMode::AUTO);
            break;
        case HelperMode::directMode:
            ghp->set_helper_mode(one::clproto::HelperMode::FORCE_DIRECT);
            break;
        case HelperMode::proxyMode:
            ghp->set_helper_mode(one::clproto::HelperMode::FORCE_PROXY);
    }

    return msg;
}

} // namespace fuse
} // namespace messages
} // namespace one
