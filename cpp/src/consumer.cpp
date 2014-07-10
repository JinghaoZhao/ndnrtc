//
//  consumer.cpp
//  ndnrtc
//
//  Copyright 2013 Regents of the University of California
//  For licensing details see the LICENSE file.
//
//  Author:  Peter Gusev
//

#include "consumer.h"
#include "frame-buffer.h"
#include "pipeliner.h"
#include "chase-estimation.h"
#include "buffer-estimator.h"
#include "rtt-estimation.h"
#include "playout.h"

using namespace std;
using namespace ndnlog;
using namespace ndnrtc;
using namespace ndnrtc::new_api;

//******************************************************************************
#pragma mark - construction/destruction
Consumer::Consumer(const ParamsStruct& params,
                   const shared_ptr<InterestQueue>& interestQueue,
                   const shared_ptr<RttEstimation>& rttEstimation):
ndnrtc::NdnRtcObject(params),
isConsuming_(false),
interestQueue_(interestQueue),
rttEstimation_(rttEstimation),
chaseEstimation_(new ChaseEstimation()),
bufferEstimator_(new BufferEstimator()),
dataMeterId_(NdnRtcUtils::setupDataRateMeter(10)),
segmentFreqMeterId_(NdnRtcUtils::setupFrequencyMeter(10))
{
    if (!rttEstimation.get())
    {
        rttEstimation_.reset(new RttEstimation());
    }
    
    bufferEstimator_->setRttEstimation(rttEstimation_);
    bufferEstimator_->setMinimalBufferSize(params.jitterSize);
}

Consumer::~Consumer()
{
    if (isConsuming_)
        stop();
}

//******************************************************************************
#pragma mark - public
int
Consumer::init()
{
    int res = RESULT_OK;
    
    if (!interestQueue_.get() ||
        !rttEstimation_.get())
        return notifyError(-1, "");
    
    frameBuffer_.reset(new ndnrtc::new_api::FrameBuffer(shared_from_this()));
    frameBuffer_->setLogger(logger_);
    frameBuffer_->setDescription(NdnRtcUtils::toString("%s-buffer",
                                                       getDescription().c_str()));
    
    res = frameBuffer_->init();

    if (RESULT_FAIL(res))
        notifyError(-1, "can't initialize frame buffer");
    
#warning error handling!
    chaseEstimation_->setLogger(logger_);
    
    pipeliner_.reset(new Pipeliner(shared_from_this()));
    pipeliner_->setLogger(logger_);
    pipeliner_->setDescription(NdnRtcUtils::toString("%s-pipeliner",
                                                     getDescription().c_str()));
    pipeliner_->registerCallback(this);
    
    renderer_->init();
    
    return res;
}

int
Consumer::start()
{
#warning error handling!
    pipeliner_->start();
    
    return RESULT_OK;
}

int
Consumer::stop()
{
#warning error handling!
    pipeliner_->stop();
    playout_->stop();
    renderer_->stopRendering();
    
    return RESULT_OK;
}

void
Consumer::reset()
{
}

void
Consumer::triggerRebuffering()
{
    pipeliner_->triggerRebuffering();
}

Consumer::State
Consumer::getState() const
{
    switch (pipeliner_->getState()) {
        case Pipeliner::StateBuffering: // fall through
        case Pipeliner::StateChasing:
            return Consumer::StateChasing;
            
        case Pipeliner::StateFetching:
            return Consumer::StateFetching;

        default:
            return Consumer::StateInactive;
    }
}

void
Consumer::getStatistics(ReceiverChannelPerformance& stat)
{
    stat.segNumDelta_ = pipeliner_->getAvgSegNum(false);
    stat.segNumKey_ = pipeliner_->getAvgSegNum(true);
    stat.rtxNum_ = pipeliner_->getRtxNum();
    stat.rtxFreq_ = pipeliner_->getRtxFreq();
    stat.rebufferingEvents_ = pipeliner_->getRebufferingNum();
    stat.rttEstimation_ = rttEstimation_->getCurrentEstimation();
    
    stat.jitterPlayableMs_ = frameBuffer_->getPlayableBufferSize();
    stat.jitterEstimationMs_ = frameBuffer_->getEstimatedBufferSize();
    stat.jitterTargetMs_ = frameBuffer_->getTargetSize();
    
    stat.segmentsFrequency_ = NdnRtcUtils::currentFrequencyMeterValue(segmentFreqMeterId_);
    stat.nBytesPerSec_ = NdnRtcUtils::currentDataRateMeterValue(dataMeterId_);
    stat.actualProducerRate_ = frameBuffer_->getCurrentRate();
    
    playout_->getStatistics(stat);
    interestQueue_->getStatistics(stat);
    frameBuffer_->getStatistics(stat);
}

void
Consumer::setLogger(ndnlog::new_api::Logger *logger)
{
    if (frameBuffer_.get())
        frameBuffer_->setLogger(logger);
    
    if (pipeliner_.get())
        pipeliner_->setLogger(logger);
    
    if (playout_.get())
        playout_->setLogger(logger);
    
    interestQueue_->setLogger(logger);
    rttEstimation_->setLogger(logger);
    chaseEstimation_->setLogger(logger);
    bufferEstimator_->setLogger(logger);
    
    ILoggingObject::setLogger(logger);
}

void
Consumer::setDescription(const std::string &desc)
{
    rttEstimation_->setDescription(NdnRtcUtils::toString("%s-rtt-est",
                                                         desc.c_str()));
    chaseEstimation_->setDescription(NdnRtcUtils::toString("%s-chase-est",
                                                           desc.c_str()));
    bufferEstimator_->setDescription(NdnRtcUtils::toString("%s-buffer-est",
                                                           desc.c_str()));
    
    ILoggingObject::setDescription(desc);
}

void
Consumer::onBufferingEnded()
{   
    if (!playout_->isRunning())
        playout_->start();
    
    if (!renderer_->isRendering())
        renderer_->startRendering(string(params_.producerId));
}

void
Consumer::onRebufferingOccurred()
{
    playout_->stop();
    renderer_->stopRendering();
}

//******************************************************************************
#pragma mark - private
void Consumer::onData(const shared_ptr<const Interest>& interest,
            const shared_ptr<Data>& data)
{
    LogTraceC << "data " << data->getName() << endl;
    
    NdnRtcUtils::dataRateMeterMoreData(dataMeterId_, data->getContent().size());
    NdnRtcUtils::frequencyMeterTick(segmentFreqMeterId_);
    
    frameBuffer_->newData(*data);
}
void Consumer::onTimeout(const shared_ptr<const Interest>& interest)
{
    LogTraceC << "timeout " << interest->getName() << endl;
    
    frameBuffer_->interestTimeout(*interest);
}
