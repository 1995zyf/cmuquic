// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/tools/quic/quic_client.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "base/logging.h"
#include "net/base/net_util.h"
#include "net/quic/crypto/quic_random.h"
#include "net/quic/quic_connection.h"
#include "net/quic/quic_data_reader.h"
#include "net/quic/quic_protocol.h"
#include "net/quic/quic_server_id.h"
#include "net/tools/epoll_server/epoll_server.h"
#include "net/tools/quic/quic_epoll_connection_helper.h"
#include "net/tools/quic/quic_socket_utils.h"

#ifndef SO_RXQ_OVFL
#define SO_RXQ_OVFL 40
#endif

using std::string;
using std::vector;

namespace net {
namespace tools {

/*
 * EPOLLIN  -- Data other than high-priority data can be read
 * EPOLLOUT -- Normal data can be written
 * EPOLLET  -- Employ edge-triggered event notification
 */
const int kEpollFlags = EPOLLIN | EPOLLOUT | EPOLLET;

QuicClient::QuicClient(IPEndPoint server_address,
                       const QuicServerId& server_id,
                       const QuicVersionVector& supported_versions,
                       EpollServer* epoll_server)
    : server_address_(server_address),
      server_id_(server_id),
      local_port_(0),
      epoll_server_(epoll_server),
      fd_(-1),
      helper_(new QuicEpollConnectionHelper(epoll_server_)),
      initialized_(false),
      packets_dropped_(0),
      overflow_supported_(false),
      supported_versions_(supported_versions) {
}

QuicClient::~QuicClient() {
  Disconnect();
}

bool QuicClient::Initialize() {
  DCHECK(!initialized_);

  // If an initial flow control window has not explicitly been set, then use the
  // same value that Chrome uses: 10 Mb.
  const uint32 kInitialFlowControlWindow = 10 * 1024 * 1024;  // 10 Mb
  if (config_.GetInitialStreamFlowControlWindowToSend() ==
      kMinimumFlowControlSendWindow) {
    config_.SetInitialStreamFlowControlWindowToSend(kInitialFlowControlWindow);
  }
  if (config_.GetInitialSessionFlowControlWindowToSend() ==
      kMinimumFlowControlSendWindow) {
    config_.SetInitialSessionFlowControlWindowToSend(kInitialFlowControlWindow);
  }

  epoll_server_->set_timeout_in_us(50 * 1000);

  if (!CreateUDPSocket()) {
    return false;
  }
  epoll_server_->RegisterFD(fd_, this, kEpollFlags);
  initialized_ = true;
  return true;
}

QuicClient::DummyPacketWriterFactory::DummyPacketWriterFactory(
    QuicPacketWriter* writer)
    : writer_(writer) {}

QuicClient::DummyPacketWriterFactory::~DummyPacketWriterFactory() {}

QuicPacketWriter* QuicClient::DummyPacketWriterFactory::Create(
    QuicConnection* /*connection*/) const {
  return writer_;
}

bool QuicClient::CreateUDPSocket() {
  int address_family = server_address_.GetSockAddrFamily();

  fd_ = netdpsock_socket(address_family, SOCK_DGRAM, IPPROTO_UDP);
  if (fd_ < 0) {
    LOG(ERROR) << "CreateSocket() failed: " << strerror(errno);
    return false;
  }

  /* (xingda) Even though we are setting the SO_RXQ_OVFL option here we aren't
   * able to use it netdp socket. */
  int get_overflow = 1;
  int rc = netdpsock_setsockopt(fd_, SOL_SOCKET, SO_RXQ_OVFL, &get_overflow,
                      sizeof(get_overflow));
  if (rc < 0) {
    DLOG(WARNING) << "Socket overflow detection not supported";
  } else {
    overflow_supported_ = true;
  }

  /* Set Send and Receive buffer size */
  if (!QuicSocketUtils::SetReceiveBufferSize(fd_,
                                             kDefaultSocketReceiveBuffer)) {
    return false;
  }

  if (!QuicSocketUtils::SetSendBufferSize(fd_, kDefaultSocketReceiveBuffer)) {
    return false;
  }

  /* (xingdaz) This is saying we want the destination ip address info on the 
   * incoming UDP packet. It works in linux socket but not on netdp ones. */
  rc = QuicSocketUtils::SetGetAddressInfo(fd_, address_family);
  if (rc < 0) {
    LOG(ERROR) << "IP detection not supported" << strerror(errno);
    return false;
  }

  if (bind_to_address_.size() != 0) {
    /* (xingdaz) this is never the case */
    client_address_ = IPEndPoint(bind_to_address_, local_port_);
  } else if (address_family == AF_INET) {
    IPAddressNumber any4 = 
      (IPAddressNumber) std::vector<unsigned char>{0,0,0,0};
    client_address_ = IPEndPoint(any4, local_port_);
  } else {
    IPAddressNumber any6 = 
      (IPAddressNumber) std::vector<unsigned char>
      {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    client_address_ = IPEndPoint(any6, local_port_);
  }

  sockaddr_storage raw_addr;
  socklen_t raw_addr_len = sizeof(raw_addr);
  CHECK(client_address_.ToSockAddr(reinterpret_cast<sockaddr*>(&raw_addr),
                                   &raw_addr_len));
  rc = netdpsock_bind(fd_, (sockaddr *)(&raw_addr), raw_addr_len);
  if (rc < 0) {
    LOG(ERROR) << "Bind failed: " << strerror(errno);
    return false;
  }

  /* (TODO) (xingdaz) netdpsock_getsockname may have a bug in that it gives 
   * the wrong sin_family = 528. So to get the netdp assigned ephemeral port, 
   * we need to assume for IPv4 and ignore IPv6 and just use sockaddr_in and 
   * reset client_address_ */
  sockaddr_in eph_addr;
  socklen_t eph_addr_len = sizeof(sockaddr_in);
  IPAddressNumber eph_any4 = 
    (IPAddressNumber) std::vector<unsigned char>{0,0,0,0};
  if (netdpsock_getsockname(fd_, (sockaddr *) (&eph_addr), &eph_addr_len) != 0){
    LOG(ERROR) << "Can't get socket name  Error: " << strerror(errno);
  }
  client_address_ = IPEndPoint(eph_any4, eph_addr.sin_port);

  return true;
}

bool QuicClient::Connect() {
  QuicPacketWriter* writer = new QuicDefaultPacketWriter(fd_);
  DummyPacketWriterFactory factory(writer);

  session_.reset(new QuicClientSession(
      config_,
      new QuicConnection((QuicConnectionId) QuicRandom::GetInstance()->RandUint64(),
                         server_address_, helper_.get(),
                         factory,
                         /* owns_writer= */ false,
                         /* is_server */ Perspective::IS_CLIENT,
                         server_id_.is_https(), supported_versions_)));

  // Reset |writer_| after |session_| so that the old writer outlives the old
  // session.
  if (writer_.get() != writer) {
    writer_.reset(writer);
  }

  session_->InitializeSession(server_id_, &crypto_config_);
  session_->CryptoConnect();

  while (!session_->IsEncryptionEstablished() &&
         session_->connection()->connected()) {
    epoll_server_->WaitForEventsAndExecuteCallbacks();
  }
  return session_->connection()->connected();
}

void QuicClient::Disconnect() {

  if (connected()) {
    session_->connection()->SendConnectionClose(QUIC_PEER_GOING_AWAY);
  }

  if (fd_ > -1) {
    epoll_server_->UnregisterFD(fd_);
    netdpsock_close(fd_);
    fd_ = -1;
  }

  initialized_ = false;
}

QuicClientStream* QuicClient::CreateClientStream() {
  if (!connected()) {
    return nullptr;
  }
  return session_->CreateClientStream();
}

void QuicClient::WaitForEvents() {
  epoll_server_->WaitForEventsAndExecuteCallbacks();
}

void QuicClient::OnEvent(int fd, EpollEvent* event) {
  DCHECK_EQ(fd, fd_);

  if (event->in_events & EPOLLIN) {
    while (connected() && ReadAndProcessPacket()) {
    }
  }
  if (connected() && (event->in_events & EPOLLOUT)) {
    writer_->SetWritable();
    session_->connection()->OnCanWrite();
  }
  if (event->in_events & EPOLLERR) {
    DVLOG(1) << "Epollerr";
  }
}

bool QuicClient::connected() const {
  return session_.get() && session_->connection() &&
      session_->connection()->connected();
}

bool QuicClient::ReadAndProcessPacket() {
  // Allocate some extra space so we can send an error if the server goes over
  // the limit.
  char buf[2 * kMaxPacketSize];

  IPEndPoint server_address;

  IPAddressNumber client_ip = client_address_.address();

  int bytes_read = QuicSocketUtils::ReadPacket(
        fd_, buf, arraysize(buf),
        overflow_supported_ ? &packets_dropped_ : nullptr, &client_ip,
        &server_address);

  if (bytes_read < 0) {
    return false;
  }

  QuicEncryptedPacket packet(buf, bytes_read, false);

  IPEndPoint client_address(client_ip, client_address_.port());
  session_->connection()->ProcessUdpPacket(
      client_address, server_address, packet);
  return true;
}

}  // namespace tools
}  // namespace net
