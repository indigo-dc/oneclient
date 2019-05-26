/**
 * @file remoteHandler.h
 * @author Krzysztof Trzepla
 * @copyright (C) 2016 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#ifndef ONECLIENT_EVENTS_HANDLERS_REMOTE_HANDLER_H
#define ONECLIENT_EVENTS_HANDLERS_REMOTE_HANDLER_H

#include "handler.h"

namespace one {
namespace client {
namespace events {

/**
 * @c LocalHandler is responsible for serialization and forwarding of aggregated
 * events to the remote subscriber.
 */
template <class T> class RemoteHandler : public Handler<T> {
public:
    /**
     * Constructor.
     * @param sequencerStream An @c communication::StreamManager::Stream
     * instance used to send serialized events.
     */
    RemoteHandler(SequencerStreamPtr sequencerStream);

    /**
     * Serializes and sends aggregated events. Skips empty events collection.
     * @see Handler::process(Events<T> events)
     */
    void process(Events<T> events) override;

private:
    SequencerStreamPtr m_sequencerStream;
};

template <class T>
RemoteHandler<T>::RemoteHandler(SequencerStreamPtr sequencerStream)
    : m_sequencerStream{std::move(sequencerStream)}
{
}

template <class T> void RemoteHandler<T>::process(Events<T> events)
{
    LOG_FCALL();

    if (!events.empty()) {
        auto clientMsg = std::make_unique<one::clproto::ClientMessage>();
        auto eventsMsg = clientMsg->mutable_events();

        for (auto &event : events) {
            auto eventMsg = eventsMsg->add_events();
            eventMsg->Swap(event->serializeAndDestroy().release());
        }

        m_sequencerStream->send(std::move(clientMsg));
    }
}

} // namespace events
} // namespace client
} // namespace one

#endif // ONECLIENT_EVENTS_HANDLERS_REMOTE_HANDLER_H
