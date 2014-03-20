//
//  rtt-estimator.cpp
//  ndnrtc
//
//  Copyright 2013 Regents of the University of California
//  For licensing details see the LICENSE file.
//
//  Author:  Peter Gusev
//

#include "rtt-estimation.h"
#include "ndnrtc-utils.h"

using namespace ndnrtc::new_api;

RttEstimation RttEstimation::sharedRttEstimation_ = RttEstimation();
const double RttEstimation::RttStartEstimate = 30; // millseconds

//******************************************************************************
#pragma mark - construction/destruction
RttEstimation::RttEstimation():
estimatorId_(NdnRtcUtils::setupMeanEstimator(0, RttStartEstimate))
{
    
}

RttEstimation::~RttEstimation()
{
}

//******************************************************************************
#pragma mark - public
RttEstimation&
RttEstimation::sharedInstance()
{
    return sharedRttEstimation_;
}

double
RttEstimation::updateEstimation(int64_t expressTime, int64_t consumeTime,
                                int64_t generationDelay)
{
    double rawValue = consumeTime-expressTime-generationDelay;
    
    if (rawValue > 0)
    {
        NdnRtcUtils::meanEstimatorNewValue(estimatorId_, rawValue);
        
        LogTrace("rtt.log") << "new RTT " <<
        NdnRtcUtils::currentMeanEstimation(estimatorId_) << endl;
    }
    else
    {
        LogWarn("") << "wrong data for RTT estimation "
        << expressTime << consumeTime << generationDelay << endl;
    }
    
    return NdnRtcUtils::currentMeanEstimation(estimatorId_);
}

double
RttEstimation::getCurrentEstimation() const
{
    return NdnRtcUtils::currentMeanEstimation(estimatorId_);
}