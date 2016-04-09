// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/tools/quic/quic_socket_utils.h"

#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <string>
#include <cstdio> 

#include "base/basictypes.h"
#include "base/logging.h"
#include "net/quic/quic_protocol.h"

#ifndef SO_RXQ_OVFL
#define SO_RXQ_OVFL 40
#endif

namespace net {
namespace tools {

/* (xingdaz) This is not used as netdp has no recvmsg API */
/**
 * Loops through cmsg linked list and find IPXX_PKTINFO
 */
IPAddressNumber QuicSocketUtils::GetAddressFromMsghdr(struct msghdr* hdr) {
  if (hdr->msg_controllen > 0) {
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(hdr);
         cmsg != nullptr;
         cmsg = CMSG_NXTHDR(hdr, cmsg)) {
      const uint8* addr_data = nullptr;
      int len = 0;
      if (cmsg->cmsg_type == IPV6_PKTINFO) {
        in6_pktinfo* info = reinterpret_cast<in6_pktinfo*>CMSG_DATA(cmsg);
        addr_data = reinterpret_cast<const uint8*>(&info->ipi6_addr);
        len = sizeof(in6_addr);
      } else if (cmsg->cmsg_type == IP_PKTINFO) {
        in_pktinfo* info = reinterpret_cast<in_pktinfo*>CMSG_DATA(cmsg);
        addr_data = reinterpret_cast<const uint8*>(&info->ipi_addr);
        len = sizeof(in_addr);
      } else {
        continue;
      }
      return IPAddressNumber(addr_data, addr_data + len);
    }
  }
  DCHECK(false) << "Unable to get address from msghdr";
  return IPAddressNumber();
}

/* (xingdaz) This is not used as netdp has no recvmsg API */
/**
 * Loops through cmsg linked list and find the SO_RXQ_OVFL data
 */
bool QuicSocketUtils::GetOverflowFromMsghdr(struct msghdr* hdr,
                                            QuicPacketCount* dropped_packets) {
  if (hdr->msg_controllen > 0) {
    struct cmsghdr* cmsg;
    for (cmsg = CMSG_FIRSTHDR(hdr);
         cmsg != nullptr;
         cmsg = CMSG_NXTHDR(hdr, cmsg)) {
      if (cmsg->cmsg_type == SO_RXQ_OVFL) {
        *dropped_packets = *(reinterpret_cast<int*>CMSG_DATA(cmsg));
        return true;
      }
    }
  }
  return false;
}

/**
 * From man page:
 *
 * Pass an IP_PKTINFO ancillary message that contains a pktinfo
 * structure that supplies some information about the incoming
 * packet.  This only works for datagram oriented sockets.  The
 * argument is a flag that tells the socket whether the
 * IP_PKTINFO message should be passed or not.  The message
 * itself can only be sent/retrieved as control message with a
 * packet using recvmsg(2) or sendmsg(2).
 *
 *     struct in_pktinfo {
 *         unsigned int   ipi_ifindex;  // Interface index
 *         struct in_addr ipi_spec_dst; // Local address
 *         struct in_addr ipi_addr;     // Header Destination
 *                                         address
 *     };
 *
 * ipi_ifindex is the unique index of the interface the packet
 * was received on.  ipi_spec_dst is the local address of the
 * packet and ipi_addr is the destination address in the packet
 * header.  If IP_PKTINFO is passed to sendmsg(2) and
 * ipi_spec_dst is not zero, then it is used as the local source
 * address for the routing table lookup and for setting up IP
 * source route options.  When ipi_ifindex is not zero, the
 * primary local address of the interface specified by the index
 * overwrites ipi_spec_dst for the routing table lookup. 
 */
int QuicSocketUtils::SetGetAddressInfo(int fd, int address_family) {
  int get_local_ip = 1;
  int rc = netdpsock_setsockopt(fd, IPPROTO_IP, IP_PKTINFO,
                      &get_local_ip, sizeof(get_local_ip));
  if (rc == 0 && address_family == AF_INET6) {
    rc = netdpsock_setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO,
                    &get_local_ip, sizeof(get_local_ip));
  }
  return rc;
}

bool QuicSocketUtils::SetSendBufferSize(int fd, size_t size) {
  if (netdpsock_setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) != 0) {
    LOG(ERROR) << "Failed to set socket send size";
    return false;
  }
  return true;
}

bool QuicSocketUtils::SetReceiveBufferSize(int fd, size_t size) {
  if (netdpsock_setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) != 0) {
    LOG(ERROR) << "Failed to set socket recv size";
    return false;
  }
  return true;
}

int QuicSocketUtils::ReadPacket(int fd, char* buffer, size_t buf_len,
                                QuicPacketCount* dropped_packets,
                                IPAddressNumber* self_address,
                                IPEndPoint* peer_address) {
  DCHECK(peer_address != nullptr);
 
  /* (xingdaz) can't use all the jazz below */
#if 0
  /* Make room for control message. CMSG_SPACE is used to make appropriate
   * amount of room for the cmsghdr struct. It also takes care of alignment. In
   * this case, we are making room for both the overflow information and the
   * destination info from the UDP packet */
  const int kSpaceForOverflowAndIp =
      CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(in6_pktinfo));
  char cbuf[kSpaceForOverflowAndIp];
  memset(cbuf, 0, arraysize(cbuf));

  iovec iov = {buffer, buf_len};
  struct sockaddr_storage raw_address;
  msghdr hdr;

  hdr.msg_name = &raw_address;
  hdr.msg_namelen = sizeof(sockaddr_storage);
  hdr.msg_iov = &iov;
  hdr.msg_iovlen = 1;
  hdr.msg_flags = 0;

  struct cmsghdr* cmsg = (struct cmsghdr*)cbuf;
  cmsg->cmsg_len = arraysize(cbuf);
  hdr.msg_control = cmsg;
  hdr.msg_controllen = arraysize(cbuf);

  int bytes_read = recvmsg(fd, &hdr, 0);
#endif

  struct sockaddr_storage raw_address;
  socklen_t address_len = sizeof(raw_address);
  int bytes_read = netdpsock_recvfrom(fd, buffer, buf_len, 0, 
                                      (sockaddr *) &raw_address, &address_len);
  
  // Return before setting dropped packets: if we get EAGAIN, it will
  // be 0.
  /* (xingdaz) We have to catch netdp's error and translate it back normal error
   * so that the calling function can interpret it. */
  if (bytes_read < 0 && errno != 0) {
    if (errno != NETDP_EAGAIN) {
      LOG(ERROR) << "Error reading " << strerror(errno);
    } else {
      errno = EAGAIN;
    }
    return -1;
  }

  /* (xingdaz) might be a prob. netdp can't tell me about dropped packet */
#if 0
  /* Tells you how many packets were dropped since we received last */
  if (dropped_packets != nullptr) {
    GetOverflowFromMsghdr(&hdr, dropped_packets);
  }
  if (self_address != nullptr) {
    *self_address = QuicSocketUtils::GetAddressFromMsghdr(&hdr);
  }
#endif

  /* Populate peer address */
  if (raw_address.ss_family == AF_INET) {
    CHECK(peer_address->FromSockAddr(
        reinterpret_cast<const sockaddr*>(&raw_address),
        sizeof(struct sockaddr_in)));
  } else if (raw_address.ss_family == AF_INET6) {
    CHECK(peer_address->FromSockAddr(
        reinterpret_cast<const sockaddr*>(&raw_address),
        sizeof(struct sockaddr_in6)));
  }

  return bytes_read;
}

/* (xingdaz) Not used */
size_t QuicSocketUtils::SetIpInfoInCmsg(const IPAddressNumber& self_address,
                                        cmsghdr* cmsg) {
  if (GetAddressFamily(self_address) == ADDRESS_FAMILY_IPV4) {
    cmsg->cmsg_len = CMSG_LEN(sizeof(in_pktinfo));
    cmsg->cmsg_level = IPPROTO_IP;
    cmsg->cmsg_type = IP_PKTINFO;
    in_pktinfo* pktinfo = reinterpret_cast<in_pktinfo*>(CMSG_DATA(cmsg));
    memset(pktinfo, 0, sizeof(in_pktinfo));
    pktinfo->ipi_ifindex = 0;
    memcpy(&pktinfo->ipi_spec_dst, &self_address[0], self_address.size());
    return sizeof(in_pktinfo);
  } else {
    cmsg->cmsg_len = CMSG_LEN(sizeof(in6_pktinfo));
    cmsg->cmsg_level = IPPROTO_IPV6;
    cmsg->cmsg_type = IPV6_PKTINFO;
    in6_pktinfo* pktinfo = reinterpret_cast<in6_pktinfo*>(CMSG_DATA(cmsg));
    memset(pktinfo, 0, sizeof(in6_pktinfo));
    memcpy(&pktinfo->ipi6_addr, &self_address[0], self_address.size());
    return sizeof(in6_pktinfo);
  }
}

WriteResult QuicSocketUtils::WritePacket(int fd,
                                         const char* buffer,
                                         size_t buf_len,
                                         const IPAddressNumber& self_address,
                                         const IPEndPoint& peer_address) {
  sockaddr_storage raw_address;
  socklen_t address_len = sizeof(raw_address);
  CHECK(peer_address.ToSockAddr(
        reinterpret_cast<struct sockaddr*>(&raw_address),
        &address_len));
  /* (xingdaz) can't use sendmsg function because there isn't the
   * equivalent of that in netdp stack. */
#if 0
  iovec iov = {const_cast<char*>(buffer), buf_len};

  msghdr hdr;
  hdr.msg_name = &raw_address;
  hdr.msg_namelen = address_len;
  hdr.msg_iov = &iov;
  hdr.msg_iovlen = 1;
  hdr.msg_flags = 0;

  const int kSpaceForIpv4 = CMSG_SPACE(sizeof(in_pktinfo));
  const int kSpaceForIpv6 = CMSG_SPACE(sizeof(in6_pktinfo));
  // kSpaceForIp should be big enough to hold both IPv4 and IPv6 packet info.
  const int kSpaceForIp =
      (kSpaceForIpv4 < kSpaceForIpv6) ? kSpaceForIpv6 : kSpaceForIpv4;
  char cbuf[kSpaceForIp];
  if (self_address.empty()) {
    hdr.msg_control = 0;
    hdr.msg_controllen = 0;
  } else {
    hdr.msg_control = cbuf;
    hdr.msg_controllen = kSpaceForIp;
    cmsghdr* cmsg = CMSG_FIRSTHDR(&hdr);
    SetIpInfoInCmsg(self_address, cmsg);
    hdr.msg_controllen = cmsg->cmsg_len;
  }
  int rc = sendmsg(fd, &hdr, 0);
#endif

  ssize_t rc = netdpsock_sendto(fd, buffer, buf_len, 0,
                                (sockaddr *) &raw_address, address_len);

  /* (xingdaz) Again, we have to catch netdp error and translate it back */
  if (rc >= 0) {
    return WriteResult(WRITE_STATUS_OK, rc);
  }
  return WriteResult((errno == NETDP_EAGAIN || errno == NETDP_EWOULDBLOCK) ?
      WRITE_STATUS_BLOCKED : WRITE_STATUS_ERROR, errno);
}

}  // namespace tools

/* (xingdaz) not used */
SockaddrStorage::SockaddrStorage(const SockaddrStorage& other)
    : addr_len(other.addr_len),
      addr(reinterpret_cast<struct sockaddr*>(&addr_storage)) {
  memcpy(addr, other.addr, addr_len);
}

void SockaddrStorage::operator=(const SockaddrStorage& other) {
  addr_len = other.addr_len;
  // addr is already set to &this->addr_storage by default ctor.
  memcpy(addr, other.addr, addr_len);
}

}  // namespace net
