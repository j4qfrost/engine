// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_COMMON_CPP_CLIENT_WRAPPER_INCLUDE_FLUTTER_EVENT_CHANNEL_H_
#define FLUTTER_SHELL_PLATFORM_COMMON_CPP_CLIENT_WRAPPER_INCLUDE_FLUTTER_EVENT_CHANNEL_H_

#include <iostream>
#include <string>
#include <atomic>
#include <thread>

#include "binary_messenger.h"
#include "engine_method_result.h"
#include "method_call.h"
#include "method_codec.h"
#include "method_result.h"

namespace flutter {

template <typename T>
class EventSink;

template<typename T>
struct StreamHandler {
  std::function<MethodResult<T>*(const T*, const EventSink<T>*)> OnListen;
  std::function<MethodResult<T>*(const T*)> OnCancel;
};

// A channel for communicating with the Flutter engine using invocation of
// asynchronous methods.
template <typename T>
class EventChannel {
 public:
  // Creates an instance that sends and receives method calls on the channel
  // named |name|, encoded with |codec| and dispatched via |messenger|.
  EventChannel(BinaryMessenger* messenger,
                const std::string& name)
      : messenger_(messenger), name_(name), codec_(new MethodCodec<T>()) {
    active_sink_.store(nullptr);
  }
  // Creates an instance that sends and receives method calls on the channel
  // named |name|, encoded with |codec| and dispatched via |messenger|.
  EventChannel(BinaryMessenger* messenger,
                const std::string& name,
                const MethodCodec<T>* codec)
      : messenger_(messenger), name_(name), codec_(codec) {
    active_sink_.store(nullptr);
  }

  ~EventChannel() = default;

  // Prevent copying.
  EventChannel(EventChannel const&) = delete;
  EventChannel& operator=(EventChannel const&) = delete;

  // Registers a handler that should be called any time a method call is
  // received on this channel.
  void SetStreamHandler(StreamHandler<T> handler) {
    const auto* codec = codec_;
    std::string channel_name = name_;
    BinaryMessageHandler binary_handler = [handler, codec, channel_name, event_channel = this](
                                              const uint8_t* message,
                                              const size_t message_size,
                                              BinaryReply reply) {
      // Use this channel's codec to decode the call and build a result handler.
      auto result =
          std::make_unique<EngineMethodResult<T>>(std::move(reply), codec);
      std::unique_ptr<MethodCall<T>> method_call =
          codec->DecodeMethodCall(message, message_size);
      if (!method_call) {
        std::cerr << "Unable to construct method call from message on channel "
                  << channel_name << std::endl;
        result->NotImplemented();
        return;
      }

      std::string method_name = method_call->method_name();
      if (method_name.compare("listen") == 0) {
        EventSink<T>* eventSink = new EventSink<T>(event_channel);
        EventSink<T>* oldSink = event_channel->active_sink_.exchange(eventSink);
        if (oldSink != nullptr) {
          // Repeated calls to onListen may happen during hot restart.
          // We separate them with a call to onCancel.
          try {
              handler.OnCancel(method_call->arguments());
          } catch (...) {
              std::cerr << channel_name << "Failed to close existing event stream\n";
          }
        }
        try {
          std::thread(handler.OnListen, method_call->arguments(), event_channel->active_sink_.load());
        } catch (...) {
            event_channel->active_sink_.store(nullptr);
            std::cerr << channel_name << "Failed to open event stream\n";
            result->Error("Failed to open event stream");
        }
      } else if (method_name.compare("cancel") == 0) {
        EventSink<T>* oldSink = event_channel->active_sink_.exchange(nullptr);
        if (oldSink != nullptr) {
            try {
                handler.OnCancel(method_call->arguments());
                result->Success();
            } catch (...) {
              std::cerr << "Failed to close event stream\n";
              result->Error("Failed to close event stream");
            }
        } else {
            result->Error("No active stream to cancel");
        }
      }

    };
    messenger_->SetMessageHandler(name_, std::move(binary_handler));
  }

  friend class EventSink<T>;

 private:
  BinaryMessenger* messenger_;
  std::string name_;
  const MethodCodec<T>* codec_;
  std::atomic<EventSink<T>*> active_sink_; 
};

template <typename T>
class EventSink
{
public:
  EventSink(EventChannel<T>* event_channel) : event_channel_(event_channel) {
    has_ended_.store(false);
  }
  ~EventSink();
  
  void Success(const T* event) const {
    if (has_ended_.load() || event_channel_->active_sink_.load() != this) {
         return;
    }
    auto message = event_channel_->codec_->EncodeSuccessEnvelope(event);
    event_channel_->messenger_->Send(event_channel_->name_, std::move(&(*message)[0]), message->size());
  }

  void Error(std::string errorCode, std::string errorMessage, const T* errorDetails) const {
    if (has_ended_.load() || event_channel_->active_sink_.load() != this) {
      return;
    }
    auto message = event_channel_->codec_->EncodeErrorEnvelope(errorCode, errorMessage, errorDetails);
    event_channel_->messenger_->Send(event_channel_->name_, std::move(&(*message)[0]), message->size());
  }


 void EndOfStream() const {
   if (has_ended_.exchange(true) || event_channel_->active_sink_.load() != this) {
      return;
   }
   event_channel_->messenger_->Send(event_channel_->name_, nullptr, 0);
 }
private:
  std::atomic<bool> has_ended_;
  EventChannel<T>* event_channel_;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_COMMON_CPP_CLIENT_WRAPPER_INCLUDE_FLUTTER_EVENT_CHANNEL_H_