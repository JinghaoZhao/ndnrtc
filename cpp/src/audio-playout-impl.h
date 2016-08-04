// 
// audio-playout-impl.h
//
//  Created by Peter Gusev on 03 August 2016.
//  Copyright 2013-2016 Regents of the University of California
//

#ifndef __audio_playout_impl_h__
#define __audio_playout_impl_h__

#include "playout-impl.h"
#include "ndnrtc-common.h"
#include "frame-buffer.h"
#include "frame-buffer.h"
#include "webrtc-audio-channel.h"

namespace ndnrtc {
  class AudioRenderer;
  
  class AudioPlayoutImpl : public PlayoutImpl
  {
  	typedef statistics::StatisticsStorage StatStorage;
  public:
    AudioPlayoutImpl(boost::asio::io_service& io,
            const boost::shared_ptr<IPlaybackQueue>& queue,
            const boost::shared_ptr<StatStorage>& statStorage = 
            boost::shared_ptr<StatStorage>(StatStorage::createConsumerStatistics()));
    ~AudioPlayoutImpl(){}

    void start(unsigned int devIdx = 0, 
      WebrtcAudioChannel::Codec codec = WebrtcAudioChannel::Codec::G722);
    void stop();

  private:
    AudioBundleSlot bundleSlot_;
    PacketNumber packetCount_;
    boost::shared_ptr<AudioRenderer> renderer_;

    void
    processSample(const boost::shared_ptr<const BufferSlot>&);
  };
}


#endif