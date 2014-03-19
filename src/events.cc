/**
 * @file veilfs.cc
 * @author Rafal Slota
 * @copyright (C) 2013 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in 'LICENSE.txt'
 */

#include "glog/logging.h"
#include "events.h"
#include "veilfs.h"
#include "communication_protocol.pb.h"

using namespace veil::client;
using namespace std;
using namespace boost;
using namespace veil::protocol::fuse_messages;

EventCommunicator::EventCommunicator() : m_eventsStream(new EventStreamCombiner())
{
}

void EventCommunicator::getEventProducerConfig()
{
    using namespace veil::protocol::communication_protocol;
    
    ClusterMsg clm;
    clm.set_protocol_version(PROTOCOL_VERSION);
    clm.set_synch(true);
    clm.set_module_name(RULE_MANAGER);
    clm.set_message_type(ATOM);
    clm.set_answer_type(EVENT_PRODUCER_CONFIG);
    clm.set_message_decoder_name(COMMUNICATION_PROTOCOL);
    clm.set_answer_decoder_name(FUSE_MESSAGES);

    Atom msg;
    msg.set_value(EVENT_PRODUCER_CONFIG_REQUEST);
    clm.set_input(msg.SerializeAsString());

    shared_ptr<CommunicationHandler> connection = VeilFS::getConnectionPool()->selectConnection();

    Answer ans;
    if(!connection || (ans=connection->communicate(clm, 0)).answer_status() == VEIO) {
        LOG(WARNING) << "sending message failed: " << (connection ? "failed" : "not needed");
    } else {
        VeilFS::getConnectionPool()->releaseConnection(connection);
        LOG(INFO) << "event producer config request sent";
    }

    LOG(INFO) << "Answer from event producer config request: " << ans.worker_answer();

    EventProducerConfig config;
    config.ParseFromString(ans.worker_answer());

    for(int i=0; i<config.event_streams_configs_size(); ++i)
    {
        addEventSubstream(config.event_streams_configs(i));
    }
}

void EventCommunicator::sendEvent(shared_ptr<EventMessage> eventMessage)
{
	using namespace veil::protocol::communication_protocol;
    string encodedEventMessage = eventMessage->SerializeAsString();
    
    ClusterMsg clm;
    clm.set_protocol_version(PROTOCOL_VERSION);
    clm.set_synch(false);
    clm.set_module_name(CLUSTER_RENGINE);
    clm.set_message_type(EVENT_MESSAGE);
    clm.set_answer_type(ATOM);
    clm.set_message_decoder_name(FUSE_MESSAGES);
    clm.set_answer_decoder_name(COMMUNICATION_PROTOCOL);

    clm.set_input(encodedEventMessage);

    LOG(INFO) << "Event message created";

    shared_ptr<CommunicationHandler> connection = VeilFS::getConnectionPool()->selectConnection();

    LOG(INFO) << "Connection selected";

    Answer ans;
    if(!connection || (ans=connection->communicate(clm, 0)).answer_status() == VEIO) {
        LOG(WARNING) << "sending message failed: " << (connection ? "failed" : "not needed");
    } else {
        VeilFS::getConnectionPool()->releaseConnection(connection);
        LOG(INFO) << "Event message sent";
    }
}

bool EventCommunicator::handlePushedConfig(const PushMessage & pushMsg){
	EventStreamConfig eventStreamConfig;
    if(!eventStreamConfig.ParseFromString(pushMsg.data())){
    	LOG(WARNING) << "Cannot parse pushed message as EventStreamConfig";
    	return false;
    }

    addEventSubstream(eventStreamConfig);
    return true;
}

void EventCommunicator::addEventSubstream(const EventStreamConfig & eventStreamConfig)
{
	shared_ptr<IEventStream> newStream = IEventStreamFactory::fromConfig(eventStreamConfig);
    if(newStream){
        m_eventsStream->addSubstream(newStream);
        LOG(INFO) << "New EventStream added.";
    }
}

void EventCommunicator::processEvent(shared_ptr<Event> event){
	if(!event){
		m_eventsStream->pushEventToProcess(event);
		VeilFS::getScheduler()->addTask(Job(time(NULL) + 1, m_eventsStream, ISchedulable::TASK_PROCESS_EVENT));
	}
}

shared_ptr<Event> Event::createMkdirEvent(string userId, string fileId)
{
	shared_ptr<Event> event (new Event());
	event->properties["type"] = string("mkdir_event");
	event->properties["userId"] = userId;
	event->properties["fileId"] = fileId;
	return event;
}

shared_ptr<Event> Event::createWriteEvent(string userId, string fileId, long long bytes)
{
	shared_ptr<Event> event (new Event());
	event->properties["type"] = string("write_event");
	event->properties["userId"] = userId;
	event->properties["fileId"] = fileId;
	event->properties["bytes"] = bytes;
	return event;
}

shared_ptr<EventMessage> Event::createProtoMessage()
{
	shared_ptr<EventMessage> eventMessage (new EventMessage());
	string type = getProperty("type", string(""));
	eventMessage->set_type(type);
	return eventMessage;
}

Event::Event(){}

Event::Event(const Event & anotherEvent)
{
	properties = anotherEvent.properties;
}

TrivialEventStream::TrivialEventStream()
{
}

shared_ptr<Event> TrivialEventStream::actualProcessEvent(shared_ptr<Event> event)
{
	shared_ptr<Event> newEvent (new Event(*event.get()));
	return newEvent;
}


shared_ptr<Event> IEventStream::processEvent(shared_ptr<Event> event)
{
	if(m_wrappedStream){
		shared_ptr<Event> processedEvent = m_wrappedStream->processEvent(event);
		if(processedEvent)
			return actualProcessEvent(processedEvent);
		else
			return shared_ptr<Event>();
	}else{
		return actualProcessEvent(event);
	}
}

IEventStream::IEventStream() :
	m_wrappedStream(shared_ptr<IEventStream>())
{
}

IEventStream::IEventStream(shared_ptr<IEventStream> wrappedStream) :
	m_wrappedStream(wrappedStream)
{
}

IEventStream::~IEventStream(){

}

void IEventStream::setWrappedStream(shared_ptr<IEventStream> wrappedStream)
{
	m_wrappedStream = wrappedStream;
}

shared_ptr<IEventStream> IEventStream::getWrappedStream()
{
	return m_wrappedStream;
}

/****** EventFilter ******/

EventFilter::EventFilter(string fieldName, string desiredValue) :
	IEventStream(), m_fieldName(fieldName), m_desiredValue(desiredValue)
{
}

EventFilter::EventFilter(shared_ptr<IEventStream> wrappedStream, std::string fieldName, std::string desiredValue) :
	IEventStream(wrappedStream), m_fieldName(fieldName), m_desiredValue(desiredValue)
{
}

shared_ptr<IEventStream> EventFilter::fromConfig(const EventFilterConfig & config)
{
	return shared_ptr<IEventStream> (new EventFilter(config.field_name(), config.desired_value()));
}

shared_ptr<Event> EventFilter::actualProcessEvent(shared_ptr<Event> event)
{
	// defaultValue is generated some way because if we set precomputed value it will not work if desiredValue is the same as precomputed value
	string defaultValue = m_desiredValue + "_";
	string value = event->getProperty(m_fieldName, defaultValue);

	if(value == m_desiredValue){
		shared_ptr<Event> newEvent (new Event(*event.get()));
		return newEvent;
	}else{
		return shared_ptr<Event>();
	}
}

string EventFilter::getFieldName()
{
	return m_fieldName;
}

string EventFilter::getDesiredValue()
{
	return m_desiredValue;
}

/****** EventAggregator ******/

EventAggregator::EventAggregator(long long threshold) :
	IEventStream(), m_fieldName(""), m_threshold(threshold)
{
}

EventAggregator::EventAggregator(std::string fieldName, long long threshold) :
	IEventStream(), m_fieldName(fieldName), m_threshold(threshold)
{
}

EventAggregator::EventAggregator(boost::shared_ptr<IEventStream> wrappedStream, long long threshold) :
	IEventStream(wrappedStream), m_threshold(threshold)
{
}

EventAggregator::EventAggregator(boost::shared_ptr<IEventStream> wrappedStream, std::string fieldName, long long threshold) :
	IEventStream(wrappedStream), m_fieldName(fieldName), m_threshold(threshold)
{
}

shared_ptr<IEventStream> EventAggregator::fromConfig(const EventAggregatorConfig & config){
	return shared_ptr<IEventStream> (new EventAggregator(config.field_name(), config.threshold()));
}

/*EventAggregator::EventAggregator(shared_ptr<IEventStream> wrappedStream, long long threshold) :
	m_wrappedStream(), m_threshold(threshold), m_counter(0)
{
}*/

shared_ptr<Event> EventAggregator::actualProcessEvent(shared_ptr<Event> event)
{
	string value;
	if(m_fieldName.empty())
		value = "";
	else{
		value = event->getProperty(m_fieldName, string(""));

		// we simply ignores events without field on which we aggregate
		if(value == "")
			return shared_ptr<Event>();
	}

	if(m_substreams.find(value) == m_substreams.end())
		m_substreams[value] = EventAggregator::ActualEventAggregator();

	return m_substreams[value].processEvent(event, m_threshold, m_fieldName);
}

string EventAggregator::getFieldName()
{
	return m_fieldName;
}

long long EventAggregator::getThreshold()
{
	return m_threshold;
}

EventAggregator::ActualEventAggregator::ActualEventAggregator() :
	m_counter(0)
{}

shared_ptr<Event> EventAggregator::ActualEventAggregator::processEvent(shared_ptr<Event> event, long long threshold, string fieldName)
{
	long long count = event->getProperty<long long>("count", 1);
	m_counter += count;

	bool forward = m_counter >= threshold;

	if(forward){
		shared_ptr<Event> newEvent (new Event());
		newEvent->properties["count"] = m_counter;
		if(!fieldName.empty()){
			string value = event->getProperty(fieldName, string());
			newEvent->properties[fieldName] = value;
		}
		resetState();
		return newEvent;
	}

	return shared_ptr<Event>();
}

void EventAggregator::ActualEventAggregator::resetState()
{
	m_counter = 0;
}

list<shared_ptr<Event> > EventStreamCombiner::processEvent(shared_ptr<Event> event)
{
	list<shared_ptr<Event> > producedEvents;
	for(list<shared_ptr<IEventStream> >::iterator it = m_substreams.begin(); it != m_substreams.end(); it++){
		shared_ptr<Event> produced = (*it)->processEvent(event);
		if(produced)
			producedEvents.push_back(produced);
	}
	return producedEvents;
}

bool EventStreamCombiner::runTask(TaskID taskId, string arg0, string arg1, string arg2)
{
	switch(taskId){
	case TASK_PROCESS_EVENT:
		return nextEventTask();

	default:
		return false;
	}
}

bool EventStreamCombiner::nextEventTask(){
	shared_ptr<Event> event = getNextEventToProcess();
	if(event){
		list<boost::shared_ptr<Event> > processedEvents = processEvent(event);
		LOG(INFO) << "event processed";

	    if(!processedEvents.empty()){
	        LOG(INFO) << "processedEvents not empty";
	    }else{
	        LOG(INFO) << "processedEvents empty";
	    }

	    for(list<boost::shared_ptr<Event> >::iterator it = processedEvents.begin(); it != processedEvents.end(); ++it){
	        LOG(INFO) << "processedEvent not null";
	        shared_ptr<EventMessage> eventProtoMessage = (*it)->createProtoMessage();
	        LOG(INFO) << "eventmessage created";

	        EventCommunicator::sendEvent(eventProtoMessage);
	    }
    }

    return true;
}

void EventStreamCombiner::pushEventToProcess(shared_ptr<Event> eventToProcess){
	m_eventsToProcess.push(eventToProcess);
}

shared_ptr<Event> EventStreamCombiner::getNextEventToProcess(){
	if(m_eventsToProcess.empty()){
		return shared_ptr<Event>();
	}

	shared_ptr<Event> event = m_eventsToProcess.front();
	m_eventsToProcess.pop();
	return event;
}

void EventStreamCombiner::addSubstream(boost::shared_ptr<IEventStream> substream){
	m_substreams.push_back(substream);
}
