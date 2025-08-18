// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

#include "InfoLoggerDispatch.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

////////////////////////////////////////////////////////
// class InfoLoggerDispatchFluentBit implementation
////////////////////////////////////////////////////////

#define FLB_RETRY_CONNECT 1 // Fluent Bit connect retry time

class InfoLoggerDispatchFluentBitImpl
{
 public:
  ConfigInfoLoggerServer* config = nullptr; 
  SimpleLog* log = nullptr;
  int sock = -1;

  void connectSink()
  {
    if (sock != -1) return;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
      log->warning("Failed to create socket for Fluent Bit: %s", strerror(errno));
      return;
    }

    sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(config->flbPort);
    if (inet_pton(AF_INET, config->flbHost.c_str(), &dst.sin_addr) != 1) {
      log->warning("Failed to convert Fluent Bit host address: %s", strerror(errno));
      close(sock);
      sock = -1;
      return;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) < 0) {
      log->warning("Failed to connect to Fluent Bit: %s", strerror(errno));
      close(sock);
      sock = -1;
      return;
    }

    log->info("Connected to Fluent Bit at %s:%d", config->flbHost.c_str(), config->flbPort);
  }

  void disconnectSink() 
  {
    if (sock != -1) {
      close(sock);
      sock = -1;
      log->info("Disconnected from Fluent Bit");
    }
  }
};

InfoLoggerDispatchFluentBit::InfoLoggerDispatchFluentBit(ConfigInfoLoggerServer* theConfig, SimpleLog* theLog, std::string prefix) : InfoLoggerDispatch(theConfig, theLog, prefix)
{
  dPtr = std::make_unique<InfoLoggerDispatchFluentBitImpl>();
  dPtr->config = theConfig;
  dPtr->log = theLog ? theLog : &defaultLog;

  dPtr->connectSink();
  if (dPtr->sock < 0) {
    dPtr->log->warning("Failed to connect to Fluent Bit. Please check your configuration.");
  }
  else {
    dPtr->log->info("Fluent Bit dispatch initialized with prefix: %s", prefix.c_str());
  }

  isReady = true;
}

InfoLoggerDispatchFluentBit::~InfoLoggerDispatchFluentBit()
{
  dPtr->disconnectSink();
}

int InfoLoggerDispatchFluentBit::customMessageProcess(std::shared_ptr<InfoLoggerMessageList> msg)
{

  static constexpr size_t DISPATCH_BUFFER_SIZE = 32768;

  if (!isReady) {
    dPtr->log->warning("Fluent Bit dispatch is not ready");
    return -1;
  }

  infoLog_msg_t* lmsg;
  char flbBuffer[DISPATCH_BUFFER_SIZE];
  size_t size_m;
  ssize_t result;

  for (lmsg = msg->msg; lmsg != nullptr; lmsg = lmsg->next) {
    if (infoLog_msg_encode(lmsg, flbBuffer, DISPATCH_BUFFER_SIZE, -1) != 0){
      dPtr->log->warning("Failed to encode message for Fluent Bit");
      continue;
    }
    
    size_m = strlen(flbBuffer);
    result = send(dPtr->sock, flbBuffer, size_m, MSG_NOSIGNAL);
    if (result < 0) {
      dPtr->log->warning("Failed to send message to Fluent Bit: %s", strerror(errno));
      dPtr->disconnectSink(); // force reconnect on next message
      return -1;
    }
  }
  return 0;
}

int InfoLoggerDispatchFluentBit::customLoop()
{
  if (dPtr->sock < 0) {
    sleep(FLB_RETRY_CONNECT);
    dPtr->log->info("Retrying connection to Fluent Bit...");
    dPtr->connectSink();
    if (dPtr->sock > 0) {
      dPtr->log->info("Connected to Fluent Bit");
    }
  }
  return 0;
}
  
