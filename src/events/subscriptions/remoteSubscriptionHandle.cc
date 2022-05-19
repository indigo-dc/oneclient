/**
 * @file remoteSubscriptionHandle.cc
 * @author Krzysztof Trzepla
 * @copyright (C) 2016 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#include "remoteSubscriptionHandle.h"
#include "helpers/logging.h"

#include "messages.pb.h"

namespace one {
namespace client {
namespace events {

RemoteSubscriptionHandle::RemoteSubscriptionHandle(StreamKey streamKey,
    Streams &streams, std::int64_t subscriptionId, ProtoSubscriptionPtr msg,
    SequencerStream &stream)
    : SubscriptionHandle(streamKey, streams)
    , m_subscriptionId{subscriptionId}
    , m_stream{stream}
{
    LOG_DBG(2) << "Sending subscription with ID: '" << subscriptionId << "'";

    auto clientMsg = std::make_unique<ProtoClient>();
    msg->set_id(m_subscriptionId);
    clientMsg->mutable_subscription()->Swap(msg.release());
    m_stream.sendSync(std::move(clientMsg));
}

RemoteSubscriptionHandle::~RemoteSubscriptionHandle()
{
    LOG_DBG(2) << "Sending cancellation for subscription with ID: '"
               << m_subscriptionId << "'";

    auto clientMsg = std::make_unique<ProtoClient>();
    auto *msg = clientMsg->mutable_subscription_cancellation();
    msg->set_id(m_subscriptionId);
    m_stream.sendSync(std::move(clientMsg));
}

} // namespace events
} // namespace client
} // namespace one
