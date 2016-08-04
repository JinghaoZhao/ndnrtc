//
//  audio-playout.h
//  ndnrtc
//
//  Created by Peter Gusev on 4/14/14.
//  Copyright 2013-2015 Regents of the University of California
//

#ifndef __ndnrtc__audio_playout__
#define __ndnrtc__audio_playout__

#include "playout.h"
#include "webrtc-audio-channel.h"

namespace ndnrtc {
  class AudioRenderer;
  class AudioPlayoutImpl;
  
  class AudioPlayout : public Playout
  {
    typedef statistics::StatisticsStorage StatStorage;
  public:
    AudioPlayout(boost::asio::io_service& io,
            const boost::shared_ptr<IPlaybackQueue>& queue,
            const boost::shared_ptr<StatStorage>& statStorage = 
            boost::shared_ptr<StatStorage>(StatStorage::createConsumerStatistics()));

    void start(unsigned int devIdx = 0, 
      WebrtcAudioChannel::Codec codec = WebrtcAudioChannel::Codec::G722);
    void stop();

  private:
    AudioPlayoutImpl* pimpl();
  };
}

#endif
