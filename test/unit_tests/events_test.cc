/**
 * @file events_test.cc
 * @author Michal Sitko
 * @copyright (C) 2014 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in 'LICENSE.txt'
 */

#include "testCommon.h"

#include "events/events.h"

#include "events_mock.h"
#include "boost/shared_ptr.hpp"
#include "fuse_messages.pb.h"
#include "communication_protocol.pb.h"
#include "options_mock.h"
#include "jobScheduler_mock.h"
#include "fslogicProxy_proxy.h"

using namespace boost;

INIT_AND_RUN_ALL_TESTS(); // TEST RUNNER !

// TEST definitions below

class EventsTest
    : public ::testing::Test
{
public:
    COMMON_DEFS();

    virtual void SetUp() {
        COMMON_SETUP();
        boost::shared_ptr<MockCommunicationHandler> connectionMock;
        connectionMock.reset(new MockCommunicationHandler());
        EXPECT_CALL(*connectionPool, selectConnection(_)).WillRepeatedly(Return(connectionMock));
        EXPECT_CALL(*connectionPool, releaseConnection(_)).WillRepeatedly(Return());
        veil::protocol::communication_protocol::Answer ans;
        ans.set_answer_status(VOK);
        EXPECT_CALL(*connectionMock, communicate(_, _, _)).WillRepeatedly(Return(ans));
    }

    virtual void TearDown() {
        COMMON_CLEANUP();
    }
};

class TestHelper
{
public:
    boost::shared_ptr<Event> processEvent(boost::shared_ptr<Event> event){
        boost::shared_ptr<Event> newEvent(new Event());
        //Event * newEvent = new Event();
        newEvent->setStringProperty("customActionKey", "custom_action_invoked");
        return newEvent;
    }
};

// checks simple stream with single EventFilter
TEST(EventFilter, SimpleFilter) {
    // given
    boost::shared_ptr<Event> mkdirEvent = Event::createMkdirEvent("file1");
    boost::shared_ptr<Event> writeEvent = Event::createWriteEvent("file2", 100);
    EventFilter filter("type", "mkdir_event");

    // what
    boost::shared_ptr<Event> resEvent = filter.processEvent(writeEvent);
    ASSERT_FALSE((bool) resEvent);

    resEvent = filter.processEvent(mkdirEvent);
    ASSERT_TRUE((bool) resEvent);
    ASSERT_EQ(string("file1"), resEvent->getStringProperty("filePath", ""));
}

// checks simple stream with single EventAggregator
TEST(EventAggregatorTest, SimpleAggregation) {
    // given
    boost::shared_ptr<Event> mkdirEvent = Event::createMkdirEvent("file1");
    boost::shared_ptr<Event> writeEvent = Event::createWriteEvent("file1", 100);
    EventAggregator aggregator(5);

    // what
    for(int i=0; i<4; ++i){
        boost::shared_ptr<Event> res = aggregator.processEvent(mkdirEvent);
        ASSERT_FALSE((bool) res);
    }

    // then
    boost::shared_ptr<Event> res = aggregator.processEvent(writeEvent);
    ASSERT_TRUE((bool) res);

    ASSERT_EQ(1, res->getNumericPropertiesSize());
    ASSERT_EQ(1, res->getStringPropertiesSize());
    ASSERT_EQ("count", res->getStringProperty(SUM_FIELD_NAME, ""));
    ASSERT_EQ(5, res->getNumericProperty("count", -1));

    for(int i=0; i<4; ++i){
        boost::shared_ptr<Event> res = aggregator.processEvent(mkdirEvent);
        ASSERT_FALSE((bool) res);
    }

    res = aggregator.processEvent(writeEvent);
    ASSERT_TRUE((bool) res);
    ASSERT_EQ(1, res->getNumericPropertiesSize());
    ASSERT_EQ(1, res->getStringPropertiesSize());
    ASSERT_EQ("count", res->getStringProperty(SUM_FIELD_NAME, ""));
    ASSERT_EQ(5, res->getNumericProperty("count", -1));
}

TEST(EventAggregatorTest, AggregationByOneField) {
    // given
    boost::shared_ptr<Event> mkdirEvent = Event::createMkdirEvent("file1");
    boost::shared_ptr<Event> writeEvent = Event::createWriteEvent("file1", 100);
    EventAggregator aggregator("type", 5);

    // what
    for(int i=0; i<4; ++i){
        boost::shared_ptr<Event> res = aggregator.processEvent(mkdirEvent);
        ASSERT_FALSE((bool) res);
    }
    boost::shared_ptr<Event> res = aggregator.processEvent(writeEvent);
    ASSERT_FALSE((bool) res);

    res = aggregator.processEvent(mkdirEvent);
    ASSERT_TRUE((bool) res);
    ASSERT_EQ(1, res->getNumericPropertiesSize());
    ASSERT_EQ(2, res->getStringPropertiesSize());
    ASSERT_EQ("count", res->getStringProperty(SUM_FIELD_NAME, ""));
    ASSERT_EQ(5, res->getNumericProperty("count", -1));
    ASSERT_EQ("mkdir_event", res->getStringProperty("type", ""));

    // we are sending just 3 writeEvents because one has already been sent
    for(int i=0; i<3; ++i){
        boost::shared_ptr<Event> res = aggregator.processEvent(writeEvent);
        ASSERT_FALSE((bool) res);
    }

    res = aggregator.processEvent(mkdirEvent);
    ASSERT_FALSE((bool) res);

    res = aggregator.processEvent(writeEvent);
    ASSERT_TRUE((bool) res);
    ASSERT_EQ(1, res->getNumericPropertiesSize());
    ASSERT_EQ(2, res->getStringPropertiesSize());
    ASSERT_EQ(5, res->getNumericProperty("count", -1));
    ASSERT_EQ("write_event", res->getStringProperty("type", ""));
}

TEST(EventAggregatorTest, AggregationWithSum) {
    boost::shared_ptr<Event> smallWriteEvent = Event::createWriteEvent("file1", 5);
    boost::shared_ptr<Event> bigWriteEvent = Event::createWriteEvent("file2", 100);
    EventAggregator aggregator("type", 110, "bytes");

    boost::shared_ptr<Event> res = aggregator.processEvent(smallWriteEvent);
    ASSERT_FALSE((bool) res);
    res = aggregator.processEvent(bigWriteEvent);
    ASSERT_FALSE((bool) res);

    res = aggregator.processEvent(smallWriteEvent);
    ASSERT_TRUE((bool) res);
    ASSERT_EQ(1, res->getNumericPropertiesSize());
    ASSERT_EQ(2, res->getStringPropertiesSize());
    ASSERT_EQ(110, res->getNumericProperty("bytes", -1));
    ASSERT_EQ("write_event", res->getStringProperty("type", ""));

    res = aggregator.processEvent(smallWriteEvent);
    ASSERT_FALSE((bool) res);
    res = aggregator.processEvent(bigWriteEvent);
    ASSERT_FALSE((bool) res);

    res = aggregator.processEvent(bigWriteEvent);
    ASSERT_TRUE((bool) res);
    ASSERT_EQ(1, res->getNumericPropertiesSize());
    ASSERT_EQ(2, res->getStringPropertiesSize());
    ASSERT_EQ(205, res->getNumericProperty("bytes", -1));
    ASSERT_EQ("write_event", res->getStringProperty("type", ""));
}

// checks event filter composed with event aggregator
TEST(EventAggregatorTest, FilterAndAggregation) {
    boost::shared_ptr<Event> file1Event = Event::createMkdirEvent("file1");
    boost::shared_ptr<Event> file2Event = Event::createMkdirEvent("file2");
    boost::shared_ptr<Event> writeEvent = Event::createWriteEvent("file1", 100);
    boost::shared_ptr<Event> writeEvent2 = Event::createWriteEvent("file2", 100);
    boost::shared_ptr<IEventStream> filter(new EventFilter("type", "mkdir_event"));
    boost::shared_ptr<IEventStream> aggregator(new EventAggregator(filter, "filePath", 5));

    for(int i=0; i<4; ++i){
        boost::shared_ptr<Event> res = aggregator->processEvent(file1Event);
        ASSERT_FALSE((bool) res);
    }

    boost::shared_ptr<Event> res = aggregator->processEvent(file2Event);
    ASSERT_FALSE((bool) res);

    res = aggregator->processEvent(writeEvent);
    ASSERT_FALSE((bool) res);

    res = aggregator->processEvent(file1Event);
    ASSERT_TRUE((bool) res);
    ASSERT_EQ(1, res->getNumericPropertiesSize());
    ASSERT_EQ(2, res->getStringPropertiesSize());
    ASSERT_EQ(5, res->getNumericProperty("count", -1));
    ASSERT_EQ("file1", res->getStringProperty("filePath", ""));

    for(int i=0; i<3; ++i){
        boost::shared_ptr<Event> res = aggregator->processEvent(file2Event);
        ASSERT_FALSE((bool) res);
    }

    res = aggregator->processEvent(file2Event);
    ASSERT_TRUE((bool) res);
    ASSERT_EQ(1, res->getNumericPropertiesSize());
    ASSERT_EQ(2, res->getStringPropertiesSize());
    ASSERT_EQ(5, res->getNumericProperty("count", -1));
    ASSERT_EQ("file2", res->getStringProperty("filePath", ""));

    for(int i=0; i<5; ++i){
        boost::shared_ptr<Event> res = aggregator->processEvent(writeEvent2);
        ASSERT_FALSE((bool) res);
    }
}

TEST(EventTransformerTest, SimpleTransformation) {
    boost::shared_ptr<Event> writeEvent = Event::createWriteEvent("file1", 100);
    vector<string> fieldNames;
    fieldNames.push_back("type");
    vector<string> toReplace;
    toReplace.push_back("write_event");
    vector<string> replaceWith;
    replaceWith.push_back("write_for_stats");
    boost::shared_ptr<IEventStream> transformer(new EventTransformer(fieldNames, toReplace, replaceWith));

    boost::shared_ptr<Event> output = transformer->processEvent(writeEvent);
    ASSERT_EQ(1, output->getNumericPropertiesSize());
    ASSERT_EQ(2, output->getStringPropertiesSize());
    ASSERT_EQ("write_for_stats", output->getStringProperty("type", ""));
}

TEST_F(EventsTest, EventStreamCombiner_CombineStreams) {
    boost::shared_ptr<Event> mkdirEvent = Event::createMkdirEvent("file1");
    boost::shared_ptr<Event> writeEvent = Event::createWriteEvent("file1", 100);
    boost::shared_ptr<IEventStream> mkdirFilter(new EventFilter("type", "mkdir_event"));
    EventStreamCombiner combiner{context};
    combiner.addSubstream(mkdirFilter);

    list<boost::shared_ptr<Event> > events = combiner.processEvent(mkdirEvent);
    ASSERT_EQ(1u, events.size());

    events = combiner.processEvent(writeEvent);
    ASSERT_EQ(0u, events.size());

    boost::shared_ptr<IEventStream> writeFilter(new EventFilter("type", "write_event"));
    combiner.addSubstream(writeFilter);

    events = combiner.processEvent(writeEvent);
    ASSERT_EQ(1u, events.size());

    events = combiner.processEvent(mkdirEvent);
    ASSERT_EQ(1u, events.size());
}

TEST(IEventStream, CustomActionStreamTest){
    TestHelper testHelper;
    boost::shared_ptr<Event> writeEvent = Event::createWriteEvent("file1", 100);
    boost::shared_ptr<Event> mkdirEvent = Event::createMkdirEvent("file1");

    boost::shared_ptr<IEventStream> filter(new EventFilter("type", "mkdir_event"));
    CustomActionStream action(filter, bind(&TestHelper::processEvent, &testHelper, _1));

    boost::shared_ptr<Event> res = action.processEvent(writeEvent);
    ASSERT_FALSE((bool) res);

    res = action.processEvent(mkdirEvent);
    ASSERT_TRUE((bool) res);

    string r = res->getStringProperty("customActionKey", "");

    ASSERT_EQ("custom_action_invoked", res->getStringProperty("customActionKey", ""));
}

// checks if EventStreams are created correctly from EventStreamConfig proto buff message
// proto buff messages are not easy to mock because their methods are nonvirtual. Mocking is possible but would need
// changes in code which is not worth it
TEST(IEventStream, ConstructFromConfig1) {
    using namespace veil::protocol::fuse_messages;

    // given
    //EventFilterConfig filterConfig;
    EventStreamConfig config;
    EventFilterConfig * filterConfig = config.mutable_filter_config();
    filterConfig->set_field_name("type");
    filterConfig->set_desired_value("write_event");

    // what
    boost::shared_ptr<IEventStream> stream = IEventStreamFactory::fromConfig(config);

    // then
    ASSERT_TRUE((bool) stream);
    EventFilter * eventFilter = dynamic_cast<EventFilter *>(stream.get());
    ASSERT_TRUE(eventFilter != NULL);
    ASSERT_EQ("type", eventFilter->getFieldName());
    ASSERT_EQ("write_event", eventFilter->getDesiredValue());
    ASSERT_FALSE((bool) eventFilter->getWrappedStream());
}

TEST(IEventStream, ConstructFromConfig2) {
    using namespace veil::protocol::fuse_messages;

    // given
    EventStreamConfig config;
    EventAggregatorConfig * aggregatorConfig = config.mutable_aggregator_config();
    aggregatorConfig->set_field_name("filePath");
    aggregatorConfig->set_sum_field_name("count");
    aggregatorConfig->set_threshold(15);
    EventStreamConfig * wrappedConfig = config.mutable_wrapped_config();
    EventFilterConfig * filterConfig = wrappedConfig->mutable_filter_config();
    filterConfig->set_field_name("type");
    filterConfig->set_desired_value("write_event");

    // what
    boost::shared_ptr<IEventStream> stream = IEventStreamFactory::fromConfig(config);

    // then
    ASSERT_TRUE((bool) stream);
    EventAggregator * eventAggregator = dynamic_cast<EventAggregator *>(stream.get());
    ASSERT_TRUE(eventAggregator != NULL);
    ASSERT_EQ("filePath", eventAggregator->getFieldName());
    ASSERT_EQ("count", eventAggregator->getSumFieldName());
    ASSERT_EQ(15, eventAggregator->getThreshold());
    boost::shared_ptr<IEventStream> wrappedStream = eventAggregator->getWrappedStream();
    ASSERT_TRUE((bool) wrappedStream);
    EventFilter * eventFilter = dynamic_cast<EventFilter *> (wrappedStream.get());
    ASSERT_TRUE(eventFilter != NULL);
    ASSERT_EQ("type", eventFilter->getFieldName());
    ASSERT_EQ("write_event", eventFilter->getDesiredValue());
    ASSERT_FALSE((bool) eventFilter->getWrappedStream());
}

TEST(IEventStream, ConstructFromConfigReturnsEmptyPointerWhenConfigIncorrect){
    using namespace veil::protocol::fuse_messages;

    // given
    EventStreamConfig config;

    // what
    boost::shared_ptr<IEventStream> stream = IEventStreamFactory::fromConfig(config);

    //config was incorrect so we expect IEventStreamFactory::fromConfig to return empty boost::shared_ptr
    ASSERT_FALSE((bool) stream);
}

TEST_F(EventsTest, EventCombinerRunTask){
    boost::shared_ptr<MockEventStream> substreamMock1(new MockEventStream());
    EXPECT_CALL(*substreamMock1, processEvent(_)).WillRepeatedly(Return(Event::createMkdirEvent("file1")));
    boost::shared_ptr<Event> event(Event::createMkdirEvent("file"));
    EventStreamCombiner combiner{context};

    combiner.pushEventToProcess(event);
    ASSERT_EQ(1u, combiner.getEventsToProcess().size());

    combiner.runTask(ISchedulable::TASK_PROCESS_EVENT, "", "", "");
    ASSERT_EQ(0u, combiner.getEventsToProcess().size());

    combiner.addSubstream(substreamMock1);

    combiner.pushEventToProcess(event);
    combiner.pushEventToProcess(event);
    ASSERT_EQ(2u, combiner.getEventsToProcess().size());

    combiner.runTask(ISchedulable::TASK_PROCESS_EVENT, "", "", "");
    ASSERT_EQ(1u, combiner.getEventsToProcess().size());

    combiner.runTask(ISchedulable::TASK_PROCESS_EVENT, "", "", "");
    ASSERT_EQ(0u, combiner.getEventsToProcess().size());

    combiner.runTask(ISchedulable::TASK_PROCESS_EVENT, "", "", "");
    ASSERT_EQ(0u, combiner.getEventsToProcess().size());
}