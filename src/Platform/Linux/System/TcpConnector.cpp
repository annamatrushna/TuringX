// Copyright (c) 2021-2022, The TuringX Project
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
// 
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
// 
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
// 
// Parts of this file are originally copyright (c) 2012-2016 The Cryptonote developers

#include "TcpConnector.h"
#include <cassert>
#include <stdexcept>

#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/epoll.h>

#include <System/InterruptedException.h>
#include <System/Ipv4Address.h>
#include "Dispatcher.h"
#include "ErrorMessage.h"
#include "TcpConnection.h"

namespace System {

namespace {

struct TcpConnectorContextExt : public OperationContext {
  int connection;
};

}

TcpConnector::TcpConnector() : dispatcher(nullptr) {
}

TcpConnector::TcpConnector(Dispatcher& dispatcher) : dispatcher(&dispatcher), context(nullptr) {
}

TcpConnector::TcpConnector(TcpConnector&& other) : dispatcher(other.dispatcher) {
  if (other.dispatcher != nullptr) {
    assert(other.context == nullptr);
    context = nullptr;
    other.dispatcher = nullptr;
  }
}

TcpConnector::~TcpConnector() {
}

TcpConnector& TcpConnector::operator=(TcpConnector&& other) {
  dispatcher = other.dispatcher;
  if (other.dispatcher != nullptr) {
    assert(other.context == nullptr);
    context = nullptr;
    other.dispatcher = nullptr;
  }

  return *this;
}

TcpConnection TcpConnector::connect(const Ipv4Address& address, uint16_t port) {
  assert(dispatcher != nullptr);
  assert(context == nullptr);
  if (dispatcher->interrupted()) {
    throw InterruptedException();
  }

  std::string message;
  int connection = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (connection == -1) {
    message = "socket failed, " + lastErrorMessage();
  } else {
    sockaddr_in bindAddress;
    bindAddress.sin_family = AF_INET;
    bindAddress.sin_port = 0;
    bindAddress.sin_addr.s_addr = INADDR_ANY;
    if (bind(connection, reinterpret_cast<sockaddr*>(&bindAddress), sizeof bindAddress) != 0) {
      message = "bind failed, " + lastErrorMessage();
    } else {
      int flags = fcntl(connection, F_GETFL, 0);
      if (flags == -1 || fcntl(connection, F_SETFL, flags | O_NONBLOCK) == -1) {
        message = "fcntl failed, " + lastErrorMessage();
      } else {
        sockaddr_in addressData;
        addressData.sin_family = AF_INET;
        addressData.sin_port = htons(port);
        addressData.sin_addr.s_addr = htonl(address.getValue());
        int result = ::connect(connection, reinterpret_cast<sockaddr *>(&addressData), sizeof addressData);
        if (result == -1) {
          if (errno == EINPROGRESS) {

            ContextPair contextPair;
            TcpConnectorContextExt connectorContext;
            connectorContext.interrupted = false;
            connectorContext.context = dispatcher->getCurrentContext();
            connectorContext.connection = connection;

            contextPair.readContext = nullptr;
            contextPair.writeContext = &connectorContext;

            epoll_event connectEvent;
            connectEvent.events = EPOLLOUT | EPOLLRDHUP | EPOLLERR | EPOLLONESHOT;
            connectEvent.data.ptr = &contextPair;
            if (epoll_ctl(dispatcher->getEpoll(), EPOLL_CTL_ADD, connection, &connectEvent) == -1) {
              message = "epoll_ctl failed, " + lastErrorMessage();
            } else {
              context = &connectorContext;
              dispatcher->getCurrentContext()->interruptProcedure = [&] {
                TcpConnectorContextExt* connectorContext1 = static_cast<TcpConnectorContextExt*>(context);
                if (!connectorContext1->interrupted) {
                  if (close(connectorContext1->connection) == -1) {
                    throw std::runtime_error("TcpListener::stop, close failed, " + lastErrorMessage());
                  }

                  connectorContext1->interrupted = true;
                  dispatcher->pushContext(connectorContext1->context);
                }
              };

              dispatcher->dispatch();
              dispatcher->getCurrentContext()->interruptProcedure = nullptr;
              assert(dispatcher != nullptr);
              assert(connectorContext.context == dispatcher->getCurrentContext());
              assert(contextPair.readContext == nullptr);
              assert(context == &connectorContext);
              context = nullptr;
              connectorContext.context = nullptr;
              if (connectorContext.interrupted) {
                throw InterruptedException();
              }

              if (epoll_ctl(dispatcher->getEpoll(), EPOLL_CTL_DEL, connection, NULL) == -1) {
                message = "epoll_ctl failed, " + lastErrorMessage();
              } else {
                if((connectorContext.events & (EPOLLERR | EPOLLHUP)) != 0) {
                  int result = close(connection);
                  assert(result != -1);

                  throw std::runtime_error("TcpConnector::connect, connection failed");
                }

                int retval = -1;
                socklen_t retValLen = sizeof(retval);
                int s = getsockopt(connection, SOL_SOCKET, SO_ERROR, &retval, &retValLen);
                if (s == -1) {
                  message =  "getsockopt failed, " + lastErrorMessage();
                } else {
                  if (retval != 0) {
                    message = "getsockopt failed, " + lastErrorMessage();
                  } else {
                    return TcpConnection(*dispatcher, connection);
                  }
                }
              }
            }
          }
        } else {
          return TcpConnection(*dispatcher, connection);
        }
      }
    }

    int result = close(connection);
    assert(result != -1);
  }


  throw std::runtime_error("TcpConnector::connect, "+message);
}

}
