/**
 * @file authException.h
 * @author Konrad Zemek
 * @copyright (C) 2014 ACK CYFRONET AGH
 * @copyright This software is released under the MIT license cited in 'LICENSE.txt'
 */

#ifndef VEILCLIENT_AUTH_EXCEPTION_H
#define VEILCLIENT_AUTH_EXCEPTION_H


#include <stdexcept>

namespace veil
{
namespace client
{
namespace auth
{

/**
 * The AuthException class is responsible for representing
 * authentication-related exceptions.
 */
class AuthException: public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/**
 * The BadAccess class is responsible for representing an authentication error
 * due to bad client credentials.
 */
class BadAccess: public AuthException
{
public:
    using AuthException::AuthException;
};

} // namespace auth
} // namespace client
} // namespace veil


#endif // VEILCLIENT_AUTH_EXCEPTION_H
