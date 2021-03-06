#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include <iostream>
#include <set>

#include "mp_common.h"
#include "mp_mping.h"
#include "mp_server.h"
#include "mp_log.h"
#include "scoped_ptr.h"
#include "mlab/socket_family.h"
#include "mlab/host.h"
#include "mlab/mlab.h"
#include "mlab/protocol_header.h"
#include "mlab/server_socket.h"

namespace {
  char usage[] = 
"Usage:  mping [<switch> [<val>]]* <host>\n\
      -n <num>    Number of messages to keep in transit\n\
      -f          Loop forever (Don't increment # messages in transit)\n\
      -R <rate>   Rate at which to limit number of messages in transit\n\
      -S          Use a TCP style slowstart\n\
\n\
      -t <ttl>    Send UDP packets (instead of ICMP) with a TTL of <ttl>\n\
      -a <ttlmax> Auto-increment TTL up to ttlmax.  Forces -t\n\
\n\
      -b <len>    Message length in bytes, including IP header, etc\n\
      -b -<sel>   Loop through message sizes: -1:selected sizes\n\
                  or steps of: -2:64 -3:128 -4:256\n\
      -B <bnum>   Send <bnum> packets in burst, should smaller than <num>\n\
      -p <port>   If UDP, destination port number\n\
\n\
      -s <sport>  Server mode, liten on UDP <sport>\n\
      -4          Server mode, use IPv4\n\
      -6          Server mode, use IPv6\n\
      -c          Client mode, sending with UDP to a server running -s\n\
      -r          Print time and sequence number of every send/recv packet.\n\
                  The time is relative to the first packet sent.\n\
                  A negative sequence number indicates a recv packet.\n\
                  Be careful, there usually are huge number of packets.\n\
\n\
      -V, -d  Version, Debug (verbose)\n\
\n\
      -F <addr>   Select a source interface\n\
      <host>     Target host\n";

const int kNbTab[] = {64, 100, 500, 1000, 1500, 2000, 3000, 4000, 0};
const char *kVersion = "mping version: 2.0 (2013.06)";
const int kDefaultTTL = 255;

// when testing, only send these packets every sec
const int kMaximumOutPacketsInTest = 20;
}  // namespace

int MPing::haltf = 0;
int MPing::tick = 0;
bool MPing::timedout = false;

void MPing::ring(int signo) {
  struct sigaction sa, osa;
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = &MPing::ring;
  sa.sa_flags = SA_INTERRUPT;
  if (sigaction(SIGALRM, &sa, &osa) < 0) {
    LOG(FATAL, "sigaction SIGALRM. %s [%d]", strerror(errno), errno);
  }
  timedout = true;
  tick = 0;
}

void MPing::halt(int signo) {
  struct sigaction sa, osa;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_INTERRUPT;
  if (++haltf >= 2) {
    sa.sa_handler = 0;
    if (sigaction(SIGINT, &sa, &osa)) {
      LOG(FATAL, "sigaction SIGINT. %s [%d]", strerror(errno), errno);
    }
  } else {
    sa.sa_handler = &MPing::halt;
    if (sigaction(SIGINT, &sa, &osa) < 0) {
      LOG(FATAL, "sigaction SIGINT. %s [%d]", strerror(errno), errno);
    }
  }
}

void MPing::InitSigAlarm() {
  struct sigaction sa, osa;
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = &MPing::ring;
  sa.sa_flags = SA_INTERRUPT;
  if (sigaction(SIGALRM, &sa, &osa) < 0) {
    LOG(FATAL, "sigaction SIGALRM. %s [%d]", strerror(errno), errno);
  }
}

void MPing::InitSigInt() {
  struct sigaction sa, osa;
  sigemptyset(&sa.sa_mask);
  sa.sa_handler = &MPing::halt;
  sa.sa_flags = SA_INTERRUPT;
  if (sigaction(SIGINT, &sa, &osa) < 0) {
    LOG(FATAL, "sigaction SIGINT. %s [%d]", strerror(errno), errno);
  }
}

void MPing::RunServer() {
  MPingServer server(pkt_size, server_port, server_family);
  server.Run();
}

void MPing::RunClient() {
  if (dest_ips.empty()) {
    LOG(ERROR, "No target address.");
    return;
  }

  InitSigAlarm();

  InitSigInt();

  for (std::set<std::string>::iterator it = dest_ips.begin();
       it != dest_ips.end(); ++it) {
    MPLOG(MPLOG_DEF, "destination IP:%s", it->c_str());

    if (!GoProbing(*it)) {
      MPLOG(MPLOG_DEF, "detination IP %s fails, try next.", it->c_str());
      continue;  // The current destination address is not responding
    } else {
      break;
    }
  }
}

bool MPing::IsServerMode() const {
  return (server_port > 0);
}

bool MPing::GoProbing(const std::string& dst_addr) {
  size_t maxsize;
  start_burst = false;  // set true when win_size > burst size
  timedout = true;
  sseq = 0;
  mrseq = 0;

  maxsize = std::max(pkt_size, kMaxBuffer);

  scoped_ptr<MpingSocket> mysock(new MpingSocket);

  if (mysock->Initialize(dst_addr, src_addr, ttl, 
                         maxsize, win_size, dport, client_mode) < 0) {
    return false;
  }

  MpingStat *mystat = &mp_stat;                                                    
  mystat->SetWindowSize(win_size);

  if (print_seq_time)
    mystat->ReserveTimeSeqVectors();

  TTLLoop(mysock.get(), mystat);

  mystat->PrintStats();

  if (print_seq_time)
    mystat->PrintResearch();

// #ifdef MP_TEST
//   mystat->PrintTimeLine();
// #endif

  return true;
}

void MPing::TTLLoop(MpingSocket *sock, MpingStat *stat) {
  ASSERT(sock != NULL);
  ASSERT(stat != NULL);
  int tempttl = 1;

  if (inc_ttl == 0) {
    tempttl = ttl;
    MPLOG(MPLOG_TTL, "ttl:%d", tempttl);
  }

  for (; tempttl <= ttl; tempttl++) {
    if (haltf > 1) 
      break;

    if (ttl) {
      sock->SetSendTTL(tempttl);
    }

    if (inc_ttl > 0)
      MPLOG(MPLOG_TTL, "ttl:%d", tempttl);

    BufferLoop(sock, stat);

    if (inc_ttl > 0)
      MPLOG(MPLOG_TTL, "ttl:%d;done;From_addr:%s", 
            tempttl, sock->GetFromAddress().c_str());

    if (haltf == 1)
      haltf = 0;
  }

  if (inc_ttl == 0)
    MPLOG(MPLOG_TTL, "ttl:%d;done", tempttl-1);
}

void MPing::BufferLoop(MpingSocket *sock, MpingStat *stat) {
  ASSERT(sock != NULL);
  ASSERT(stat != NULL);

  int nbix = 0;

  if (pkt_size > 0)
    MPLOG(MPLOG_BUF, "packet_size:%zu", pkt_size);

  for (nbix = 0; ; nbix++) {
    if (haltf)
      break;

    if (pkt_size > 0) {  // set packet size: use static packet size
      cur_packet_size = pkt_size;
      if (nbix != 0) 
        break;
    } else {  // set packet size: increase packet size
      if (loop_size == -1) {
        if (kNbTab[nbix] == 0) 
          break;

        cur_packet_size = kNbTab[nbix];
      } else if (loop_size == -2) {
        if ((nbix+1)*64 > 1500)
          break;

        cur_packet_size = (nbix+1)*64;
      } else if (loop_size == -3) {
        if ((nbix+1)*128 > 2048)
          break;

        cur_packet_size = (nbix+1)*128;
      } else if (loop_size == -4) {
        if ((nbix+1)*256 > 4500)
          break;

        cur_packet_size = (nbix+1)*256;
      } else {
        LOG(FATAL, "Wrong loop throught message size %d.\n%s",
            loop_size, usage);
      }
    }  // end of set packet size

    if (loop_size < 0)
      MPLOG(MPLOG_BUF, "packet_size:%zu", cur_packet_size);

    WindowLoop(sock, stat);

    if (loop_size < 0)
      MPLOG(MPLOG_BUF, "packet_size:%zu;done", cur_packet_size);
  }

  if (pkt_size > 0)
    MPLOG(MPLOG_BUF, "packet_size:%zu;done", pkt_size);
  
}

void MPing::WindowLoop(MpingSocket *sock, MpingStat *stat) {
  ASSERT(sock != NULL);
  ASSERT(stat != NULL);
  // third loop: window size
  // no -f flag:        1,2,3,....,win_size,0,break
  // -f w/ other loops: win_size,break
  // -f no other loops: win_size,win_size,...<interrupt>,0,break
  // 0 is to collect all trailing messages still in transit
  uint16_t intran;  // current window size
  struct timeval now;

  if (loop) 
    MPLOG(MPLOG_WIN, "window_size:%d", win_size);

  for (intran = loop?win_size:1; intran; intran?intran++:0) {
    if (haltf)
      intran = 0;

    if (intran > win_size) {
      if (loop) {
        if (inc_ttl > 0 || loop_size < 0)
          break;
        else
          intran = win_size;
      } else {
        intran = 0;
      }
    }

    if (intran > 0 && timedout) {
      mustsend = 1;
      timedout = false;
    }

    // printing
    if (!loop) {
      MPLOG(MPLOG_WIN, "window_size:%d", intran);
    }

    IntervalLoop(intran, sock, stat);

    if (print_seq_time) {
      gettimeofday(&now, 0);
      stat->InsertIntervalBoundry(now);
    }

    stat->PrintTempStats();
  }

  if (loop) 
    MPLOG(MPLOG_WIN, "window_size:%d;done", win_size);
}

void MPing::IntervalLoop(uint16_t intran, MpingSocket *sock, MpingStat *stat) {
  ASSERT(sock != NULL);
  ASSERT(stat != NULL);
  struct timeval now;
  
  if (!tick) {  // sync to system clock
    gettimeofday(&now, 0);
    tick = now.tv_sec;
    while (tick >= now.tv_sec) {
      gettimeofday(&now, 0);
    }
  }

  alarm(2);  // reset each time called, recv timeout if recv block
  tick++;

#ifdef MP_TEST
  int out = 0;  
#endif        
  while (tick >= now.tv_sec) {
    int maxopen;
    int rt;
    unsigned int rseq;
    int err;
    bool timeout = false;
    int diff, need_send;

    // send
    if (burst ==  0 || !start_burst) {  // no burst
      maxopen = slow_start?2:10;
      diff = (int)(sseq - mrseq - intran);
      need_send = (diff < 0)?std::min(maxopen, (0-diff)):mustsend;
    } else {  // start burst, now we have built the window
      diff = (int)(sseq - mrseq + burst - intran);
      need_send = (diff > 0)?mustsend:burst;
    }

    mustsend = 0;

    while (need_send > 0) {
      sseq++;
      rt = sock->SendPacket(sseq, cur_packet_size, &err);

      if (rt < 0) {  // send fails
        if (err != EINTR) { 
          if (err == ENOBUFS) {
            LOG(ERROR, "send buffer run out.");
            maxopen = 0;
            sseq--;
          } else {
            if (err != ECONNREFUSED) {  // because we connect on UDP sock
              LOG(FATAL, "send fails. %s [%d]", strerror(err), err);
            } else {
              sseq--;
            }
          }
        }
        else {
          timeout = true;
          break;
        }
      } else {  // send success, update counters
        gettimeofday(&now, 0);
        
        if (print_seq_time)
          stat->InsertSequenceTime(sseq, now);

        stat->EnqueueSend(sseq, now);
#ifdef MP_TEST              
        out++;
#endif              
        
        if (burst > 0 && intran >= burst && 
            !start_burst && (sseq-mrseq-intran) == 0) {
          // let the on-flight reach window size, then start burst
          LOG(VERBOSE, "start burst, window %d, burst %d",
              intran, burst);
          start_burst = true;  // once set, stay true
        }
        need_send--;
      }
    }

    if (timeout) {
      LOG(ERROR, "send being interrupted.");
      break;  // almost never happen
    }
    
    // recv
    rseq = sock->ReceiveAndGetSeq(&err, stat);
    if (err != 0) {
      //if (err == EINTR)
       // continue;
     // else
      if (err != EINTR)
        LOG(FATAL, "recv fails. %s [%d]", strerror(err), err);
    } else {
      gettimeofday(&now, 0);

      if (print_seq_time)
        stat->InsertSequenceTime(0-rseq, now);

      stat->EnqueueRecv(rseq, now);

      if ((int)(sseq - rseq) < 0) {
        LOG(ERROR, "recv a seq larger than sent %d %d %d",
            mrseq, rseq, sseq);
      } else {
        mrseq = rseq;
      }
    }
#ifdef MP_TEST
    if (out >= kMaximumOutPacketsInTest) {
      break;
    }
#endif
    gettimeofday(&now, 0);
  }  // end of fourth loop: time tick
}

MPing::MPing(const int& argc, const char **argv) :
      win_size(4),                                                              
      loop(false),                                                              
      rate(0),                                                                  
      slow_start(false),                                                        
      ttl(0),                                                                   
      inc_ttl(0),                                                               
      pkt_size(0),                                                              
      loop_size(0),                                                             
      version(false),                                                           
      debug(false),                                                             
      burst(0),                                                                 
      interval(0),
      dport(0),
      server_port(0),
      server_family(SOCKETFAMILY_UNSPEC),
      client_mode(false),
      print_seq_time(false),
      mustsend(0),
      start_burst(false),
      sseq(0),
      mrseq(0),
      cur_packet_size(0) {
  int ac = argc;
  const char **av = argv;
  const char *p;
  bool host_set = false;

  if (argc < 2) {
    printf("%s", usage);
    exit(0);
  }

  ac--;
  av++;

  while (ac > 0) {
    p = *av++;  // p point to switch, av point to value
    if (p[0] == '-') {
      if (ac == 1) {
        switch (p[1]) {  // those switches without value
          case 'f': loop = true; av--; break;
          case 'S': slow_start = true; av--; break;
          case 'V': version = true; av--; break;
          case 'd': debug = true; av--; break;
          case 'c': client_mode = true; av--; break;
          case 'r': print_seq_time = true; av--; break;
          case '4': server_family = SOCKETFAMILY_IPV4; av--; break;
          case '6': server_family = SOCKETFAMILY_IPV6; av--; break;
          default: LOG(FATAL, "\n%s", usage); break;
        }
      } else {
        switch (p[1]) {
          case 'n': { win_size = atoi(*av); ac--; break; }
          case 'f': { loop = true; av--; break; }
          case 'R': { rate = atoi(*av); ac--; break; }
          case 'S': { slow_start = true; av--; break; }
          case 't': { ttl = atoi(*av); ac--; break; }
          case 's': { server_port = atoi(*av); ac--; break; }
          case 'a': { inc_ttl = atoi(*av); ttl = inc_ttl; ac--; break; }
          case 'b': {
            p = *av;
            if (p[0] == '-') {
              loop_size = atoi(*av);  
            } else {
              pkt_size = atoi(*av);
            }

            ac--;
            break; 
          }
          case 'p': { dport = atoi(*av); ac--; break; }
          case 'B': { burst = atoi(*av); ac--; break; }
          case 'V': { version = true; av--; break; }
          case 'd': { debug = true; av--; break; }
          case 'c': { client_mode = true; av--; break; }
          case 'r': { print_seq_time = true; av--; break; }
          case '4': { server_family = SOCKETFAMILY_IPV4; av--; break; }
          case '6': { server_family = SOCKETFAMILY_IPV6; av--; break; }
          case 'F': { src_addr = std::string(*av); ac--; break; }
          default: { 
            LOG(FATAL, "Unknown parameter -%c\n%s", p[1], usage); break; 
          }
        }
      }
    } else {  // host
      if (!host_set) {
        dst_host = std::string(p);  
        av--;
        host_set = 1;
      }else{
        LOG(FATAL, "%s, %s\n%s", p, dst_host.c_str(), usage);
      }
    }

    ac--;
    av++;
  }

  // must have set host
  // if (!host_set) {
  //   LOG(FATAL, "Must have destination host. \n%s", usage);
  // }

  ValidatePara();
}

void MPing::ValidatePara() {

  // print version
  if (version) {
    std::cout << kVersion << std::endl;
    exit(0);
  }

  // server mode
  if (server_port > 0) {
    if (server_port > 65535) {
      LOG(FATAL, "Server port cannot larger than 65535.");
    }
    
    if (server_family == SOCKETFAMILY_UNSPEC){
      LOG(FATAL, "Need to know the socket family, use -4 or -6.");
    }

    return;
  }

  if (debug) {
    SetLogSeverity(VERBOSE);
  }

  // destination set?
  if (dst_host.empty()) {
    LOG(FATAL, "Must have destination host. \n%s", usage);
  }

  // client mode
  if (client_mode) {
    if (dport == 0) {
      LOG(FATAL, "Client mode must have destination port using -p.");
    }

    if (ttl == 0) {
      ttl = kDefaultTTL;
    }
  }
  
  // TTL
  if (ttl > 255 || ttl < 0) {
    ttl = 255;
    LOG(WARNING, "TTL %d is either > 255 or < 0, " 
                       "now set TTL to 255.", ttl);
  } 

  // loop_size [-4, -1]
  if (loop_size < -4 || loop_size > 0) {
    LOG(FATAL, "loop through message size could only take"
                     "-1, -2, -3 or -4");
  }

  // inc_ttl
  if (inc_ttl > 255 || inc_ttl < 0) {
    inc_ttl = 255;
    LOG(WARNING, "Auto-increment TTL %u is either > 255 or < 0, "
                       "now set auto-increment TTL to 255.", inc_ttl);
  }

  // destination host
  mlab::Host dest(dst_host);
  if (dest.resolved_ips.empty()) {
    LOG(FATAL, "Destination host %s invalid.", dst_host.c_str());
  } else {  // set destination ip set
    dest_ips = dest.resolved_ips;
  }

  // max packet size
  if (pkt_size > 65535) {
    LOG(FATAL, "Packet size cannot larger than 65535.");
  }

  // validate local host
  if (src_addr.length() > 0) {
    if (mlab::GetSocketFamilyForAddress(src_addr) == 
            SOCKETFAMILY_UNSPEC) {
      LOG(FATAL, "Local host %s invalid. Only accept numeric IP address",
          src_addr.c_str());
    }
  } 

  // validate UDP destination port
  if (dport > 0 ) {
    if (ttl == 0 && !client_mode) {
      LOG(FATAL, "-p can only use together with -t -a or -p.\n%s", usage);
    }

    if (dport > 65535) {
      LOG(FATAL, "UDP destination port cannot larger than 65535.");
    }
  }
}
