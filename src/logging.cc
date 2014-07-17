/**
 * @file logging.cc
 * @author Konrad Zemek
 * @copyright (C) 2014 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in 'LICENSE.txt'
 */

#include "logging.h"

#include "communicationHandler.h"
#include "helpers/storageHelperFactory.h"
#include "simpleConnectionPool.h"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <google/protobuf/descriptor.h>

#include <ctime>
#include <chrono>
#include <numeric>
#include <sstream>

static constexpr std::chrono::seconds AFTER_FAIL_DELAY(2);
static constexpr std::chrono::seconds MAX_FLUSH_DELAY{10};
static const std::string CENTRAL_LOG_MODULE_NAME("central_logger");
static const std::string LOGGING_DECODER("logging");

namespace veil
{
namespace logging
{

static RemoteLogLevel glogToLevel(google::LogSeverity glevel)
{
    switch(glevel)
    {
        case google::INFO: return protocol::logging::INFO;
        case google::WARNING: return protocol::logging::WARNING;
        case google::ERROR: return protocol::logging::ERROR;
        case google::FATAL: return protocol::logging::FATAL;
        default: return protocol::logging::NONE;
    }
}

RemoteLogWriter::RemoteLogWriter(const RemoteLogLevel initialThreshold,
                                 const BufferSize maxBufferSize,
                                 const BufferSize bufferTrimSize)
    : m_pid{getpid()}
    , m_maxBufferSize{maxBufferSize}
    , m_bufferTrimSize{bufferTrimSize}
    , m_thresholdLevel{initialThreshold}
    , m_stopWriteLoop{false}
{
}

void RemoteLogWriter::run(std::shared_ptr<SimpleConnectionPool> connectionPool)
{
    if(m_thread.joinable())
    {
        LOG(WARNING) << "run called while a thread is already running";
        return;
    }

    m_connectionPool = std::move(connectionPool);
    m_thread = std::thread(&RemoteLogWriter::writeLoop, this);
}

RemoteLogWriter::~RemoteLogWriter()
{
    m_stopWriteLoop = true;
    m_bufferChanged.notify_all();
    if(m_thread.joinable())
        m_thread.join();
}

void RemoteLogWriter::buffer(const RemoteLogLevel level,
                             const std::string &fileName, const int line,
                             const time_t timestamp, const std::string &message)
{
    if(m_thresholdLevel > level) // nothing to log
        return;

    protocol::logging::LogMessage log;
    log.set_level(level);
    log.set_pid(m_pid);
    log.set_file_name(fileName);
    log.set_line(line),
    log.set_timestamp(timestamp);
    log.set_message(message);

    pushMessage(log);
}

bool RemoteLogWriter::handleThresholdChange(const protocol::communication_protocol::Answer &answer)
{
    if(!boost::algorithm::iequals(answer.message_type(), "ChangeRemoteLogLevel"))
        return true;

    protocol::logging::ChangeRemoteLogLevel req;
    req.ParseFromString(answer.worker_answer());
    m_thresholdLevel = req.level();

    LOG(INFO) << "Client will now log " << req.level() <<
                 " and higher level messages to cluster.";

    return true;
}

void RemoteLogWriter::pushMessage(const protocol::logging::LogMessage &msg)
{
    std::lock_guard<std::mutex> guard(m_bufferMutex);

    m_buffer.push(msg);

    if(m_buffer.size() > m_maxBufferSize)
        dropExcessMessages();

    m_bufferChanged.notify_all();
}

protocol::logging::LogMessage RemoteLogWriter::popMessage()
{
    std::unique_lock<std::mutex> lock(m_bufferMutex);
    while(m_buffer.empty() && !m_stopWriteLoop)
        m_bufferChanged.wait_for(lock, MAX_FLUSH_DELAY);

    if(m_stopWriteLoop)
        return protocol::logging::LogMessage{};

    const protocol::logging::LogMessage msg = m_buffer.front();
    m_buffer.pop();
    return msg;
}

void RemoteLogWriter::writeLoop()
{
    while(!m_stopWriteLoop)
    {
        if(!sendNextMessage())
            std::this_thread::sleep_for(AFTER_FAIL_DELAY);
    }
}

bool RemoteLogWriter::sendNextMessage()
{
    const protocol::logging::LogMessage msg = popMessage();
    if(m_stopWriteLoop)
        return true;

    auto connectionPool = m_connectionPool;
    if(!connectionPool)
        return false;

    auto connection = connectionPool->selectConnection();
    if(!connection)
        return false;

    protocol::communication_protocol::ClusterMsg clm;
    clm.set_protocol_version(PROTOCOL_VERSION);
    clm.set_synch(false);
    clm.set_module_name(CENTRAL_LOG_MODULE_NAME);
    clm.set_message_decoder_name(LOGGING_DECODER);
    clm.set_message_type(boost::algorithm::to_lower_copy(msg.GetDescriptor()->name()));
    clm.set_answer_type(boost::algorithm::to_lower_copy(protocol::communication_protocol::Atom::descriptor()->name()));
    clm.set_answer_decoder_name(COMMUNICATION_PROTOCOL);
    msg.SerializeToString(clm.mutable_input());

    try
    {
        connection->sendMessage(clm, IGNORE_ANSWER_MSG_ID);
        return true;
    }
    catch(CommunicationHandler::ConnectionStatus)
    {
        return false;
    }
}

void RemoteLogWriter::dropExcessMessages()
{
    const BufferSize dropped = m_buffer.size() - m_bufferTrimSize;
    while (m_buffer.size() > m_bufferTrimSize)
        m_buffer.pop();

    std::stringstream message;
    message << "RemoteLogWriter dropped " << dropped
           << " messages as the limit of " << m_maxBufferSize
           << " buffered messages has been exceeded";

    protocol::logging::LogMessage log;
    log.set_level(protocol::logging::WARNING);
    log.set_pid(m_pid);
    log.set_file_name("logging.cc");
    log.set_line(__LINE__),
    log.set_timestamp(std::time(nullptr));
    log.set_message(message.str());

    m_buffer.push(log);
}

RemoteLogSink::RemoteLogSink(std::shared_ptr<RemoteLogWriter> writer,
                             const RemoteLogLevel forcedLevel)
    : m_forcedLevel{forcedLevel}
    , m_writer{std::move(writer)}
{
}

void RemoteLogSink::send(google::LogSeverity severity,
                         const char */*full_filename*/,
                         const char *base_filename, int line, const tm *tm_time,
                         const char *message, size_t message_len)
{
    const time_t timestamp = std::mktime(const_cast<tm*>(tm_time));
    const RemoteLogLevel level = m_forcedLevel != protocol::logging::NONE
            ? m_forcedLevel : glogToLevel(severity);

    m_writer->buffer(level, base_filename, line, timestamp,
                     std::string(message, message_len));
}

std::weak_ptr<RemoteLogSink> _logSink;
std::weak_ptr<RemoteLogSink> _debugLogSink;

void setLogSinks(const std::shared_ptr<RemoteLogSink> &logSink,
                 const std::shared_ptr<RemoteLogSink> &debugLogSink)
{
    _logSink = logSink;
    _debugLogSink = debugLogSink;
}

} // namespace logging
} // namespace veil