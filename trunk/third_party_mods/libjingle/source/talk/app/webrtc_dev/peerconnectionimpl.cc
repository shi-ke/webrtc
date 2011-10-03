/*
 * libjingle
 * Copyright 2011, Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "talk/app/webrtc_dev/peerconnectionimpl.h"

#include <vector>

#include "talk/app/webrtc_dev/scoped_refptr_msg.h"
#include "talk/app/webrtc_dev/streamcollectionimpl.h"
#include "talk/base/logging.h"
#include "talk/session/phone/channelmanager.h"

namespace {

// The number of the tokens in the config string.
static const size_t kConfigTokens = 2;
static const int kServiceCount = 5;
// The default stun port.
static const int kDefaultPort = 3478;

// NOTE: Must be in the same order as the ServiceType enum.
static const char* kValidServiceTypes[kServiceCount] = {
    "STUN", "STUNS", "TURN", "TURNS", "INVALID" };

static const char kUserAgent[] = "PeerConnection User Agent";

enum ServiceType {
  STUN,     // Indicates a STUN server.
  STUNS,    // Indicates a STUN server used with a TLS session.
  TURN,     // Indicates a TURN server
  TURNS,    // Indicates a TURN server used with a TLS session.
  INVALID,  // Unknown.
};

enum {
    MSG_COMMITSTREAMCHANGES = 1,
    MSG_PROCESSSIGNALINGMESSAGE = 2,
    MSG_RETURNREMOTEMEDIASTREAMS = 3
};

bool static ParseConfigString(const std::string& config,
                              talk_base::SocketAddress* addr,
                              ServiceType* service_type) {
  std::vector<std::string> tokens;
  talk_base::tokenize(config, ' ', &tokens);

  if (tokens.size() != kConfigTokens) {
    LOG(WARNING) << "Invalid config string";
    return false;
  }

  *service_type = INVALID;

  const std::string& type = tokens[0];
  for (size_t i = 0; i < kServiceCount; ++i) {
    if (type.compare(kValidServiceTypes[i]) == 0) {
      *service_type = static_cast<ServiceType>(i);
      break;
    }
  }

  if (*service_type == INVALID) {
    LOG(WARNING) << "Invalid service type: " << type;
    return false;
  }
  std::string service_address = tokens[1];

  int port;
  tokens.clear();
  talk_base::tokenize(service_address, ':', &tokens);
  if (tokens.size() != kConfigTokens) {
    port = kDefaultPort;
  } else {
    port = atoi(tokens[1].c_str());
    if (port <= 0 || port > 0xffff) {
      LOG(WARNING) << "Invalid port: " << tokens[1];
      return false;
    }
  }
  addr->SetIP(service_address);
  addr->SetPort(port);
  return true;
}

struct SignalingParams : public talk_base::MessageData {
  SignalingParams(const std::string& msg,
                  webrtc::StreamCollection* local_streams)
      : msg(msg),
        local_streams(local_streams) {}
  const std::string msg;
  scoped_refptr<webrtc::StreamCollection> local_streams;
};

}  // namespace

namespace webrtc {

PeerConnectionImpl::PeerConnectionImpl(
    cricket::ChannelManager* channel_manager,
    talk_base::Thread* signaling_thread,
    PcNetworkManager* network_manager,
    PcPacketSocketFactory* socket_factory)
    : observer_(NULL),
      local_media_streams_(StreamCollectionImpl::Create()),
      signaling_thread_(signaling_thread),
      channel_manager_(channel_manager),
      network_manager_(network_manager),
      socket_factory_(socket_factory),
      port_allocator_(new cricket::HttpPortAllocator(
          network_manager->network_manager(),
          socket_factory->socket_factory(),
          std::string(kUserAgent))),
      signaling_(new PeerConnectionSignaling(channel_manager,
                                             signaling_thread)) {
  signaling_->SignalNewPeerConnectionMessage.connect(
      this, &PeerConnectionImpl::OnNewPeerConnectionMessage);
  signaling_->SignalRemoteStreamAdded.connect(
      this, &PeerConnectionImpl::OnRemoteStreamAdded);
  signaling_->SignalRemoteStreamRemoved.connect(
      this, &PeerConnectionImpl::OnRemoteStreamRemoved);
}

PeerConnectionImpl::~PeerConnectionImpl() {
  signaling_thread_->Clear(this);
}

bool PeerConnectionImpl::Initialize(const std::string& configuration,
                                    PeerConnectionObserver* observer) {
  ASSERT(observer);
  if (!observer)
    return false;
  observer_ = observer;
  talk_base::SocketAddress address;
  ServiceType service;
  if (!ParseConfigString(configuration, &address, &service))
    return false;

  switch (service) {
    case STUN: {
      std::vector<talk_base::SocketAddress> address_vector;
      address_vector.push_back(address);
      port_allocator_->SetStunHosts(address_vector);
      break;
    }
    case TURN: {
      std::vector<std::string> address_vector;
      address_vector.push_back(address.ToString());
      port_allocator_->SetRelayHosts(address_vector);
      break;
    }
    default:
      ASSERT(!"NOT SUPPORTED");
      return false;
  }

  return true;
}

scoped_refptr<StreamCollection> PeerConnectionImpl::local_streams() {
  return local_media_streams_;
}

scoped_refptr<StreamCollection> PeerConnectionImpl::remote_streams() {
  ScopedRefMessageData<StreamCollection>* msg =
      new ScopedRefMessageData<StreamCollection>(NULL);
  signaling_thread_->Send(this, MSG_RETURNREMOTEMEDIASTREAMS, msg);
  return msg->data();
}

bool PeerConnectionImpl::ProcessSignalingMessage(const std::string& msg) {
  SignalingParams* parameter(new SignalingParams(
      msg, StreamCollectionImpl::Create(local_media_streams_)));
  signaling_thread_->Post(this, MSG_PROCESSSIGNALINGMESSAGE, parameter);
}

void PeerConnectionImpl::AddStream(LocalMediaStream* local_stream) {
  local_media_streams_->AddStream(local_stream);
}

void PeerConnectionImpl::RemoveStream(LocalMediaStream* remove_stream) {
  local_media_streams_->RemoveStream(remove_stream);
}

void PeerConnectionImpl::CommitStreamChanges() {
  ScopedRefMessageData<StreamCollection>* msg =
      new ScopedRefMessageData<StreamCollection> (
          StreamCollectionImpl::Create(local_media_streams_));
  signaling_thread_->Post(this, MSG_COMMITSTREAMCHANGES, msg);
}

void PeerConnectionImpl::OnMessage(talk_base::Message* msg) {
  talk_base::MessageData* data = msg->pdata;
  switch (msg->message_id) {
    case MSG_COMMITSTREAMCHANGES: {
      ScopedRefMessageData<StreamCollection>* param(
          static_cast<ScopedRefMessageData<StreamCollection>*> (data));
      signaling_->CreateOffer(param->data());
      delete data;  // Because it is Posted.
      break;
    }
    case MSG_PROCESSSIGNALINGMESSAGE: {
      SignalingParams* params(static_cast<SignalingParams*> (data));
      signaling_->ProcessSignalingMessage(params->msg, params->local_streams);
      delete data;  // Because it is Posted.
      break;
    }
    case MSG_RETURNREMOTEMEDIASTREAMS: {
      ScopedRefMessageData<StreamCollection>* param(
          static_cast<ScopedRefMessageData<StreamCollection>*> (data));
      param->data() = StreamCollectionImpl::Create(remote_media_streams_);
      break;
    }
  }
}

void PeerConnectionImpl::OnNewPeerConnectionMessage(
    const std::string& message) {
  observer_->OnSignalingMessage(message);
}

void PeerConnectionImpl::OnRemoteStreamAdded(MediaStream* remote_stream) {
  // TODO(perkj): add function in pc signaling to return a collection of
  // remote streams.
  // This way we can avoid keeping a separate list of remote_media_streams_.
  remote_media_streams_->AddStream(remote_stream);
  observer_->OnAddStream(remote_stream);
}

void PeerConnectionImpl::OnRemoteStreamRemoved(MediaStream* remote_stream) {
  // TODO(perkj): add function in pc signaling to return a collection of
  // remote streams.
  // This way we can avoid keeping a separate list of remote_media_streams_.
  remote_media_streams_->RemoveStream(remote_stream);
  observer_->OnRemoveStream(remote_stream);
}

}  // namespace webrtc
