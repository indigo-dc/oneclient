/**
* @file eventManager.cc
* @author Krzysztof Trzepla
* @copyright (C) 2015 ACK CYFRONET AGH
* @copyright This software is released under the MIT license cited in
* 'LICENSE.txt'
*/

#include "context.h"
#include "communication/subscriptionData.h"

#include "events/eventManager.h"
#include "events/types/readEvent.h"
#include "events/types/writeEvent.h"
#include "events/types/truncateEvent.h"

#include "messages/readEventSubscription.h"
#include "messages/writeEventSubscription.h"
#include "messages/eventSubscriptionCancellation.h"

#include "messages.pb.h"

#include <glog/logging.h>

namespace one {
namespace client {
namespace events {

EventManager::EventManager(std::shared_ptr<Context> context)
    : m_context{std::move(context)}
{
    auto communicator = std::make_shared<EventCommunicator>(m_context);
    m_readEventStream =
        std::make_unique<EventStream<ReadEvent>>(m_context, communicator);
    m_writeEventStream =
        std::make_unique<EventStream<WriteEvent>>(m_context, communicator);

    auto predicate = [](const clproto::ServerMessage &msg, const bool) {
        return msg.has_event_subscription();
    };
    auto callback =
        [this](const clproto::ServerMessage &msg) { handleServerMessage(msg); };

    m_context->communicator()->subscribe(communication::SubscriptionData{
        std::move(predicate), std::move(callback)});
}

void EventManager::emitReadEvent(
    std::string fileId, off_t offset, size_t size) const
{
    m_context->scheduler()->post([ =, fileId = std::move(fileId) ] {
        auto event = ReadEvent{std::move(fileId), offset, size};
        LOG(INFO) << "Emitting event: " << event.toString();
        m_readEventStream->push(std::move(event));
    });
}

void EventManager::emitWriteEvent(
    std::string fileId, off_t offset, size_t size, off_t fileSize) const
{
    m_context->scheduler()->post([ =, fileId = std::move(fileId) ] {
        auto event = WriteEvent{std::move(fileId), offset, size, fileSize};
        LOG(INFO) << "Emitting event: " << event.toString();
        m_writeEventStream->push(std::move(event));
    });
}

void EventManager::emitTruncateEvent(std::string fileId, off_t fileSize) const
{
    m_context->scheduler()->post([ =, fileId = std::move(fileId) ] {
        auto event = TruncateEvent{std::move(fileId), fileSize};
        LOG(INFO) << "Emitting event: " << event.toString();
        m_writeEventStream->push(std::move(event));
    });
}

void EventManager::handleServerMessage(const clproto::ServerMessage &msg)
{
    auto subscriptionMsg = msg.event_subscription();
    if (subscriptionMsg.has_read_event_subscription()) {
        ReadEventSubscription subscription{msg};
        auto id = m_readEventStream->addSubscription(subscription);
        m_subscriptionsCancellation.emplace(
            id, [ this, subscription = std::move(subscription) ] {
                m_readEventStream->removeSubscription(std::move(subscription));
            });
    }
    else if (subscriptionMsg.has_write_event_subscription()) {
        WriteEventSubscription subscription{msg};
        auto id = m_writeEventStream->addSubscription(subscription);
        m_subscriptionsCancellation.emplace(
            id, [ this, subscription = std::move(subscription) ] {
                m_writeEventStream->removeSubscription(std::move(subscription));
            });
    }
    else if (subscriptionMsg.has_event_subscription_cancellation()) {
        EventSubscriptionCancellation cancellation{msg};
        auto searchResult = m_subscriptionsCancellation.find(cancellation.id());
        if (searchResult != m_subscriptionsCancellation.end()) {
            searchResult->second();
            m_subscriptionsCancellation.erase(searchResult);
        }
    }
}

} // namespace events
} // namespace client
} // namespace one
