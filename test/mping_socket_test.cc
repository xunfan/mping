#include "gtest/gtest.h"
#include "mlab/mlab.h"
#include "mp_socket.h"

TEST(MpingSocket, IPv4ICMP) {
  MpingSocket testsock("127.0.0.1", "", 0, 1024, 1, 0);
  testsock.SendPacket(1, 1024);
  EXPECT_EQ(1u, testsock.ReceiveAndGetSeq(1024));
}

TEST(MpingSocket, IPv4ICMPwithSrc) {
  MpingSocket test2sock("127.0.0.1", "172.17.94.151", 0, 1024, 1, 0);
  test2sock.SendPacket(2, 1024);
  EXPECT_EQ(2u, test2sock.ReceiveAndGetSeq(1024));
}

TEST(MpingSocket, IPv6ICMP) {
  MpingSocket testsock("::1", "", 0, 1024, 1, 0);

  testsock.SendPacket(3, 1024);
  EXPECT_EQ(3u, testsock.ReceiveAndGetSeq(1024));
}

TEST(MpingSocket, IPv6ICMPwithSrc) {
  MpingSocket test2sock("::1", "2620:0:1000:3202:6e3b:e5ff:fe0b:5ca3",
                        0, 1024, 1, 0);
  test2sock.SendPacket(4, 1024);
  EXPECT_EQ(4u, test2sock.ReceiveAndGetSeq(1024));
}

TEST(MpingSocket, IPv4UDP) {
  MpingSocket testsock("127.0.0.1", "", 14, 1024, 1, 0);

  testsock.SendPacket(5, 1024);
  EXPECT_EQ(5u, testsock.ReceiveAndGetSeq(1024));
}

TEST(MpingSocket, IPv4UDPwithSrc) {
  MpingSocket test2sock("127.0.0.1", "172.17.94.151", 14, 1024, 1, 0);
  test2sock.SendPacket(6, 1024);
  EXPECT_EQ(6u, test2sock.ReceiveAndGetSeq(1024));
}

TEST(MpingSocket, IPv6UDP) {
  MpingSocket testsock("::1", "", 14, 1024, 1, 0);

  testsock.SendPacket(7, 1024);
  EXPECT_EQ(7u, testsock.ReceiveAndGetSeq(1024));
}

TEST(MpingSocket, IPv6UDPwithSrc) {
  MpingSocket test2sock("::1", "2620:0:1000:3202:6e3b:e5ff:fe0b:5ca3",
                        14, 1024, 1, 0);
  test2sock.SendPacket(8, 1024);
  EXPECT_EQ(8u, test2sock.ReceiveAndGetSeq(1024));
}

