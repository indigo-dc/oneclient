/**
 * @file xattr.cc
 * @author Bartek Kryza
 * @copyright (C) 2017 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in
 * 'LICENSE.txt'
 */

#include "xattr.h"

#include "messages.pb.h"

#include <sstream>
#include <system_error>

namespace one {
namespace messages {
namespace fuse {

XAttr::XAttr(std::unique_ptr<ProtocolServerMessage> serverMessage)
    : FuseResponse{serverMessage}
{
    if (!serverMessage->fuse_response().has_xattr()) {
        throw std::system_error{std::make_error_code(std::errc::protocol_error),
            "xattr field missing"};
    }

    auto *xattr = serverMessage->mutable_fuse_response()->mutable_xattr();

    m_name = xattr->name();
    m_value = xattr->value();
}

XAttr::XAttr(std::string name, std::string value)
    : m_name{std::move(name)}
    , m_value{std::move(value)}
{
}

const std::string &XAttr::name() const { return m_name; }

const std::string &XAttr::value() const { return m_value; }

std::string XAttr::toString() const
{
    std::stringstream stream;
    stream << "type: 'XAttr', name: " << m_name << ", value: " << m_value;

    return stream.str();
}

} // namespace fuse
} // namespace messages
} // namespace one
