// 
// video-playout-impl.h
//
//  Created by Peter Gusev on 03 August 2016.
//  Copyright 2013-2016 Regents of the University of California
//

#ifndef __video_playout_impl_h__
#define __video_playout_impl_h__

#include "playout-impl.h"
#include "frame-buffer.h"

namespace ndnrtc {
	template<typename T>
    class VideoFramePacketT;
    struct Immutable;
    typedef VideoFramePacketT<Immutable> ImmutableFrameAlias;
    class IPlaybackQueue;
    class IEncodedFrameConsumer;
    class IVideoPlayoutObserver;

	class VideoPlayoutImpl : public PlayoutImpl {
        typedef statistics::StatisticsStorage StatStorage;
    public:
        VideoPlayoutImpl(boost::asio::io_service& io,
            const boost::shared_ptr<IPlaybackQueue>& queue,
            const boost::shared_ptr<StatStorage>& statStorage = 
                boost::shared_ptr<StatStorage>(StatStorage::createConsumerStatistics()));
        
        void stop();
        void registerFrameConsumer(IEncodedFrameConsumer* frameConsumer);
        void deregisterFrameConsumer();

        void attach(IVideoPlayoutObserver* observer);
        void detach(IVideoPlayoutObserver* observer);
        
    private:
        // using Playout::attach;
        // using Playout::detach;

        VideoFrameSlot frameSlot_;
        IEncodedFrameConsumer *frameConsumer_;
        bool gopIsValid_;
        PacketNumber currentPlayNo_;
        int gopCount_;

        void
        processSample(const boost::shared_ptr<const BufferSlot>&);
	};

	class IEncodedFrameConsumer 
    {
    public:
        virtual void processFrame(const boost::shared_ptr<ImmutableFrameAlias>&) = 0;
    };

    class IVideoPlayoutObserver : public IPlayoutObserver 
    {
    public:
        virtual void frameSkipped(PacketNumber pNo, bool isKey) = 0;
        virtual void frameProcessed(PacketNumber pNo, bool isKey) = 0;
        virtual void recoveryFailure(PacketNumber sampleNo, bool isKey) = 0;
    };
}

#endif
