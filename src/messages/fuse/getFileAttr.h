/**
 * @file getFileAttr.h
 * @author Konrad Zemek
 * @copyright (C) 2015 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#ifndef ONECLIENT_MESSAGES_FUSE_GET_FILE_ATTR_H
#define ONECLIENT_MESSAGES_FUSE_GET_FILE_ATTR_H

#include "fileRequest.h"

#include <folly/FBString.h>
#include <folly/Optional.h>

#include <string>

namespace one {
namespace messages {
namespace fuse {

/**
 * The GetFileAttr class represents a FUSE request for file attributes.
 */
class GetFileAttr : public FileRequest {
public:
    /**
     * Constructor.
     * @param uuid UUID of the file for which attributes are requested.
     * @param includeReplicationStatus Whether the response should include
     * replication status.
     */
    GetFileAttr(folly::fbstring uuid,
        folly::Optional<bool> includeReplicationStatus = {});

    std::string toString() const override;

private:
    std::unique_ptr<ProtocolClientMessage> serializeAndDestroy() override;

    folly::Optional<bool> m_includeReplicationStatus;
};

} // namespace fuse
} // namespace messages
} // namespace one

#endif // ONECLIENT_MESSAGES_FUSE_GET_FILE_ATTR_H
