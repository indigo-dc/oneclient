/**
 * @file getFileAttr.cc
 * @author Konrad Zemek
 * @copyright (C) 2015 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#include "getFileAttr.h"

#include "messages.pb.h"

#include <cassert>
#include <sstream>

namespace one {
namespace messages {
namespace fuse {

GetFileAttr::GetFileAttr(
    folly::fbstring uuid, folly::Optional<bool> includeReplicationStatus)
    : FileRequest{uuid.toStdString()}
    , m_includeReplicationStatus{std::move(includeReplicationStatus)}
{
}

std::string GetFileAttr::toString() const
{
    std::stringstream stream;
    stream << "type: 'GetFileAttr', uuid: " << m_contextGuid;

    if (m_includeReplicationStatus)
        stream << ", includeReplicationStatus: " << *m_includeReplicationStatus;

    return stream.str();
}

std::unique_ptr<ProtocolClientMessage> GetFileAttr::serializeAndDestroy()
{
    auto msg = FileRequest::serializeAndDestroy();
    if (m_includeReplicationStatus)
        msg->mutable_fuse_request()
            ->mutable_file_request()
            ->mutable_get_file_attr()
            ->set_include_replication_status(*m_includeReplicationStatus);

    return msg;
}

} // namespace fuse
} // namespace messages
} // namespace one
