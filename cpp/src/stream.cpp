//
// stream.cpp
//
//  Created by Peter Gusev on 26 February 2019.
//  Copyright 2013-2019 Regents of the University of California
//

#include "stream.hpp"

#include <mutex>

#include <ndn-cpp/name.hpp>
#include <ndn-cpp/data.hpp>
#include <ndn-cpp/digest-sha256-signature.hpp>
#include <ndn-cpp/security/key-chain.hpp>
#include <ndn-cpp/util/memory-content-cache.hpp>
#include <ndn-cpp-tools/usersync/content-meta-info.hpp>

#include "clock.hpp"
#include "estimators.hpp"
#include "fec.hpp"
#include "name-components.hpp"
#include "proto/ndnrtc.pb.h"
#include "video-codec.hpp"

#define PARITY_RATIO 0.2
#define NDNRTC_FRAME_TYPE "ndnrtcv4"

using namespace std;
using namespace ndn;
using namespace ndnrtc;
using namespace ndnrtc::statistics;

namespace ndnrtc {

    // TODO: move it into a separate file
    class SegmentsManifest : public Data {
    public:
        SegmentsManifest(const Name&, const vector<boost::shared_ptr<Data>>& segments);

        bool hasData(Data& d);
    };

    class LiveMetadata
    {
      public:
        LiveMetadata();
        ~LiveMetadata();

        double getRate() const { return rateMeter_.value(); }
        double getSegmentsNumEstimate(FrameType ft, SegmentClass cls) const;

        void update(bool isKey, size_t nDataSeg, size_t nParitySeg);

      private:
        LiveMetadata(const LiveMetadata &) = delete;

        estimators::FreqMeter rateMeter_;
        estimators::Average deltaData_, deltaParity_;
        estimators::Average keyData_, keyParity_;
    };

    class VideoStreamImpl2 : public NdnRtcComponent, public StatObject {
    public:
        VideoStreamImpl2(string basePrefix, string streamName, VideoStream::Settings settings,
                         boost::shared_ptr<KeyChain> kc);
        ~VideoStreamImpl2();

        string getBasePrefix() const { return basePrefix_; }
        string getStreamName() const { return streamName_; }
        string getPrefix() const {
            return Name(basePrefix_).appendTimestamp(timestamp_).append(streamName_).toUri();
        }
        StatisticsStorage getStatistics() const;

        vector<boost::shared_ptr<Data>> processImage(const ImageFormat&, uint8_t *imgData);


    private:
        string basePrefix_;
        string streamName_;
        uint64_t timestamp_;
        Name streamPrefix_;

        struct _Freshness {
            uint32_t sample_,
            keySample_,
            gop_,
            latest_,
            live_,
            meta_;
        };

        struct _Freshness freshness_;
        mutable mutex internalMutex_;

        Name lastFramePrefix_, lastGopPrefix_;
        LiveMetadata liveMetadata_;

        // encoding cycles timestamps (monotonic clock)
        uint64_t lastCycleMonotonicNs_, thisCycleMonotonicNs_;
        uint64_t lastPublishEpochMs_;
        uint64_t frameSeq_, gopPos_, gopSeq_;

        // these are the packets that were generated outside of "processImage"
        // call and will be added to the output of the next  "processImage" call
        vector<boost::shared_ptr<Data>> queued_;
        // stream metadata
        boost::shared_ptr<Data> meta_;
        // latest generated "latest_" and "live_" packets
        boost::shared_ptr<Data> latest_;
        boost::shared_ptr<Data> live_;

        VideoStream::Settings settings_;
        boost::shared_ptr<KeyChain> keyChain_;

        VideoCodec codec_;

        void addMeta();

        Name publishFrameGobj(const EncodedFrame&);
        vector<uint8_t> generateFecData(const EncodedFrame&, size_t, size_t, size_t);
        boost::shared_ptr<Data> generateFrameMeta(uint64_t,
                                                  const Name&, const FrameType &,
                                                  size_t);
        Name publishGop(const Name&, const Name&,
                        vector<boost::shared_ptr<Data>>&);

        // signs with keychain or phony signature depending on the flag passed
        void sign(Data& d, bool phony = false);
        size_t getPayloadSegmentSize() const {
            // TODO: explore if you can accommodate NDN's packet overhead here
            return settings_.segmentSize_;
        }

        void onLiveMetadataRequest(const boost::shared_ptr<const Name>& prefix,
                               const boost::shared_ptr<const Interest>& interest, Face& face,
                               uint64_t interestFilterId,
                               const boost::shared_ptr<const InterestFilter>& filter);

        void onLatestMetaRequest(const boost::shared_ptr<const Name>& prefix,
                                 const boost::shared_ptr<const Interest>& interest, Face& face,
                                 uint64_t interestFilterId,
                                 const boost::shared_ptr<const InterestFilter>& filter);

        boost::shared_ptr<Data> generateLatestPacket();
        boost::shared_ptr<Data> generateLivePacket();
    };
}

//******************************************************************************
SegmentsManifest::SegmentsManifest(const Name& name,
                                   const vector<boost::shared_ptr<Data>>& segments)
   : Data(Name(name).append(NameComponents::NameComponentManifest))
{
    const size_t DigestSize = 32;
    vector<uint8_t> payload(DigestSize*segments.size(), 0);
    uint8_t *ptr = payload.data();

    for (auto &d : segments)
    {
        Blob digest = (*d->getFullName())[-1].getValue();
        assert(digest.size() == DigestSize);

        copy(digest->begin(), digest->end(), ptr);
    }

    setContent(payload);
}

bool
SegmentsManifest::hasData(Data& d)
{
    Blob digest = (*d.getFullName())[-1].getValue();
    const uint8_t *ptr = getContent().buf();

    while (ptr - getContent().buf() < getContent().size())
    {
        Blob b(ptr, digest.size());
        if (b.equals(digest))
            return true;
        ptr += digest.size();
    }

    return false;
}

//******************************************************************************
LiveMetadata::LiveMetadata()
    : rateMeter_(estimators::FreqMeter(boost::make_shared<estimators::TimeWindow>(1000)))
    , deltaData_(estimators::Average(boost::make_shared<estimators::TimeWindow>(100)))
    , deltaParity_(estimators::Average(boost::make_shared<estimators::TimeWindow>(100)))
    , keyData_(estimators::Average(boost::make_shared<estimators::SampleWindow>(2)))
    , keyParity_(estimators::Average(boost::make_shared<estimators::SampleWindow>(2)))
{}

LiveMetadata::~LiveMetadata() {}

void
LiveMetadata::update(bool isKey, size_t nDataSeg, size_t nParitySeg)
{
    estimators::Average &dataAvg = (isKey ? keyData_ : deltaData_);
    estimators::Average &parityAvg = (isKey ? keyParity_ : deltaParity_);

    // double r = rateMeter_.value();
    // double d = deltaData_.value();
    // double dp = deltaParity_.value();
    // double k = keyData_.value();
    // double kp = keyParity_.value();

    rateMeter_.newValue(0);
    dataAvg.newValue(nDataSeg);
    parityAvg.newValue(nParitySeg);
}

double
LiveMetadata::getSegmentsNumEstimate(FrameType ft, SegmentClass cls) const
{
    if (ft == FrameType::Key)
        return (cls == SegmentClass::Data ? keyData_.value() : keyParity_.value());

    return (cls == SegmentClass::Data ? deltaData_.value() : deltaParity_.value());
}

//******************************************************************************
VideoStream::VideoStream(std::string basePrefix, std::string streamName,
    VideoStream::Settings settings, boost::shared_ptr<ndn::KeyChain> keyChain)
: pimpl_(boost::make_shared<VideoStreamImpl2>(basePrefix, streamName, settings, keyChain)){}
VideoStream::~VideoStream(){}
vector<boost::shared_ptr<Data>>
VideoStream::processImage(const ImageFormat& fmt, uint8_t *imageData)
    { return pimpl_->processImage(fmt, imageData); }
string VideoStream::getBasePrefix() const{ return pimpl_->getBasePrefix(); }
string VideoStream::getStreamName() const { return pimpl_->getStreamName(); }
string VideoStream::getPrefix() const { return pimpl_->getPrefix(); }
statistics::StatisticsStorage
VideoStream::getStatistics() const { return pimpl_->getStatistics(); }
void VideoStream::setLogger(boost::shared_ptr<ndnlog::new_api::Logger> logger)
    { pimpl_->setLogger(logger); }

boost::shared_ptr<StorageEngine>
VideoStream::getStorage() const
{
    // TODO:
    return boost::shared_ptr<StorageEngine>();
}

VideoStream::Settings&
VideoStream::defaultSettings()
{
    static VideoStream::Settings s;
    s.codecSettings_ = VideoCodec::defaultCodecSettings();
    s.segmentSize_ = 8000;
    s.useFec_ = true;
    s.storeInMemCache_ = false;

    return s;
}

//******************************************************************************
VideoStreamImpl2::VideoStreamImpl2(string basePrefix, string streamName,
    VideoStream::Settings settings, boost::shared_ptr<KeyChain> kc)
    : StatObject(boost::shared_ptr<StatisticsStorage>(StatisticsStorage::createProducerStatistics()))
    , basePrefix_(basePrefix)
    , streamName_(streamName)
    , settings_(settings)
    , keyChain_(kc)
    , timestamp_(clock::millisecondTimestamp())
    , lastCycleMonotonicNs_(0)
    , thisCycleMonotonicNs_(0)
    , frameSeq_(0)
    , gopPos_(0)
    , gopSeq_(0)
{
    description_ = "video-stream-"+streamName_;
    streamPrefix_ = Name(getPrefix());
    codec_.initEncoder(settings_.codecSettings_);

    freshness_.sample_ = 1000 / settings_.codecSettings_.spec_.encoder_.fps_;
    freshness_.keySample_ = freshness_.gop_ =
        settings_.codecSettings_.spec_.encoder_.gop_ * freshness_.sample_;
    freshness_.latest_ = freshness_.sample_;
    freshness_.live_ = freshness_.gop_;
    freshness_.meta_ = 4000; // default, whatever

    // init last frame prefix
    lastFramePrefix_ = Name(getPrefix()).appendSequenceNumber((uint64_t)-1);

    // register callbacks for live requests
    if (settings_.memCache_)
    {
        settings_.memCache_->setInterestFilter(Name(streamPrefix_).append(NameComponents::Live),
            bind(&VideoStreamImpl2::onLiveMetadataRequest, this, _1, _2, _3, _4, _5));
        settings_.memCache_->setInterestFilter(Name(streamPrefix_).append(NameComponents::Latest),
            bind(&VideoStreamImpl2::onLatestMetaRequest, this, _1, _2, _3, _4, _5));
    }

    addMeta();
}

VideoStreamImpl2::~VideoStreamImpl2()
{
    // TODO: unset interest filters callbacks
}

StatisticsStorage
VideoStreamImpl2::getStatistics() const
{
    // captured == processed in this setup as VideoSream is single-threaded
    (*statStorage_)[Indicator::ProcessedNum] =
        (*statStorage_)[Indicator::CapturedNum] = codec_.getStats().nFrames_;

    (*statStorage_)[Indicator::EncodedNum] = codec_.getStats().nProcessed_;
    (*statStorage_)[Indicator::DroppedNum] = codec_.getStats().nDropped_;

    return *statStorage_;
}

vector<boost::shared_ptr<Data>>
VideoStreamImpl2::processImage(const ImageFormat& fmt, uint8_t *imgData)
{
    LogDebugC << "⤹ incoming frame #" << frameSeq_ << std::endl;

    thisCycleMonotonicNs_ = clock::nanosecondTimestamp();
    vector<boost::shared_ptr<Data>> packets;

    // encode frame
    VideoCodec::Image raw(settings_.codecSettings_.spec_.encoder_.width_,
                          settings_.codecSettings_.spec_.encoder_.height_,
                          fmt, imgData);
    raw.setUserData((void*)&packets);

    LogDebugC << "↓ feeding #" << frameSeq_ << " into encoder..." << endl;

    boost::shared_ptr<VideoStreamImpl2> self = dynamic_pointer_cast<VideoStreamImpl2>(shared_from_this());
    codec_.encode(raw, false,
        [this, self, &packets](const EncodedFrame &frame){
            LogDebugC << "+ encoded #" << frameSeq_
                      << (frame.type_ == FrameType::Key ? " key: " : " delta: ")
                      << frame.length_ << "bytes" << endl;

            // generatePackets(frame);
            Name framePrefix = publishFrameGobj(frame);

            if (frame.type_ == FrameType::Key)
            {
                lastGopPrefix_ = publishGop(framePrefix, lastFramePrefix_, packets);
                gopSeq_++;
            }

            gopPos_++;
            frameSeq_++;
            lastFramePrefix_ = framePrefix;
        },
        [this, self](const VideoCodec::Image &dropped){
            LogWarnC << "⨂ dropped" << endl;
        });

    // at last, add queued packets and flush queued_ array
    if (queued_.size())
    {
        lock_guard<mutex> g(internalMutex_);

        copy(queued_.begin(), queued_.end(), back_inserter(packets));
        queued_.clear();
    }

    if (settings_.storeInMemCache_)
        for (auto &d : packets)
            settings_.memCache_->add(*d);

    lastCycleMonotonicNs_ = thisCycleMonotonicNs_;

    return packets;
}

void
VideoStreamImpl2::addMeta()
{
    // only one meta per stream
    assert(!meta_);

    meta_ = boost::make_shared<Data>(Name(getPrefix()).append(NameComponents::NameComponentMeta));
    meta_->getMetaInfo().setFreshnessPeriod(freshness_.meta_);

    StreamMeta meta;
    meta.set_width(settings_.codecSettings_.spec_.encoder_.width_);
    meta.set_width(settings_.codecSettings_.spec_.encoder_.height_);
    meta.set_description("description is not supported yet");

    string s = meta.SerializeAsString();
    meta_->setContent((uint8_t*)s.data(), s.size());
    sign(*meta_);
    {
        lock_guard<mutex> g(internalMutex_);
        queued_.push_back(meta_);
    }

    LogTraceC << meta_->getName() << endl;
}

Name
VideoStreamImpl2::publishFrameGobj(const EncodedFrame& frame)
{
    lastPublishEpochMs_ = ndn_getNowMilliseconds();
    Name frameName = Name(lastFramePrefix_.getPrefix(-1)).appendSequenceNumber(frameSeq_);
    vector<boost::shared_ptr<Data>> *packets = (vector<boost::shared_ptr<Data>>*)frame.userData_;
    size_t payloadSegmentSize = getPayloadSegmentSize();
    size_t nDataSegments = frame.length_ / payloadSegmentSize + (frame.length_ % payloadSegmentSize ? 1 : 0);
    size_t nParitySegments = ceil(PARITY_RATIO * nDataSegments);
    uint16_t sampleFreshness = (frame.type_ == FrameType::Key ? freshness_.keySample_ : freshness_.sample_);
    if (nParitySegments == 0) nParitySegments = 1;
    nParitySegments *= settings_.useFec_;

    Name::Component dataFinalBlockId = Name::Component::fromSegment(nDataSegments-1);
    Name::Component parityFinalBlockId = Name::Component::fromSegment(nParitySegments-1);

    // generate FEC data if needed
    vector<uint8_t> fecData;
    if (settings_.useFec_)
        fecData = move(generateFecData(frame, nDataSegments, nParitySegments, payloadSegmentSize));

    // slice encoded frame data into data packets
    uint8_t *slice = frame.data_;
    size_t lastSegSize = frame.length_ - payloadSegmentSize * (nDataSegments-1);

    for (int seg = 0; seg < nDataSegments; ++seg)
    {
        // TODO: explore NDN-CPP Lite API for zerocopy
        boost::shared_ptr<Data> d = boost::make_shared<Data>(Name(frameName).appendSegment(seg));
        d->getMetaInfo().setFreshnessPeriod(sampleFreshness);
        d->getMetaInfo().setFinalBlockId(dataFinalBlockId);
        d->setContent(slice, (seg == nDataSegments-1 ? lastSegSize : payloadSegmentSize));
        sign(*d, true);

        packets->push_back(d);
        slice += payloadSegmentSize;
    }

    slice = fecData.data();
    for (int seg = 0; seg < nParitySegments; ++seg)
    {
        boost::shared_ptr<Data> d = boost::make_shared<Data>(Name(frameName)
            .append(NameComponents::NameComponentParity).appendSegment(seg));
        d->getMetaInfo().setFreshnessPeriod(sampleFreshness);
        d->getMetaInfo().setFinalBlockId(parityFinalBlockId);
        d->setContent(slice, payloadSegmentSize);
        sign(*d, true);

        packets->push_back(d);
        slice += payloadSegmentSize;
    }

    LogTraceC << "▻▻▻ generated " << packets->size() << " segments ("
              << nDataSegments << " data " << nParitySegments << " parity)" << endl;

    packets->push_back(boost::make_shared<SegmentsManifest>(frameName, *packets));
    packets->push_back(generateFrameMeta(lastPublishEpochMs_, frameName, frame.type_, nParitySegments));

    LogDebugC << "⤷ published GObj-Frame " << frameName << endl;

    // update live meta
    liveMetadata_.update(frame.type_ == FrameType::Key, nDataSegments, nParitySegments);
    (*statStorage_)[Indicator::CurrentProducerFramerate] = liveMetadata_.getRate();

    if (frame.type_ == FrameType::Key)
        (*statStorage_)[Indicator::PublishedKeyNum]++;

    return frameName;
}

vector<uint8_t>
VideoStreamImpl2::generateFecData(const EncodedFrame &frame,
                                  size_t nDataSegments, size_t nParitySegments,
                                  size_t payloadSegSize)
{
    vector<uint8_t> fecData(nParitySegments * payloadSegSize, 0);
    fec::Rs28Encoder enc(nDataSegments, nParitySegments, payloadSegSize);
    size_t zeroPadding = nDataSegments * payloadSegSize - frame.length_;

    vector<uint8_t> paddedData(nDataSegments*payloadSegSize, 0);
    copy(frame.data_, frame.data_+frame.length_, paddedData.begin());

    if (enc.encode(paddedData.data(), fecData.data()) != 0)
    {
        LogWarnC << "! error generating FEC data" << endl;
        fecData.resize(0);
    }

    return fecData;
}

boost::shared_ptr<Data>
VideoStreamImpl2::generateFrameMeta(uint64_t nowMs,
                                    const Name &frameName, const FrameType &ft,
                                    size_t nParitySegments)
{
    // add packets for EncodedFrame as GObj
    int64_t secs = thisCycleMonotonicNs_/1E9;
    int32_t nanosec = thisCycleMonotonicNs_ - secs*1E9;
    google::protobuf::Timestamp *captureTimestamp = new google::protobuf::Timestamp();
    captureTimestamp->set_seconds(secs);
    captureTimestamp->set_nanos(nanosec);

    FrameMeta meta;
    meta.set_allocated_capture_timestamp(captureTimestamp);
    meta.set_parity_size(nParitySegments);
    meta.set_gop_number(gopSeq_);
    meta.set_gop_position(gopPos_);
    meta.set_type(ft == FrameType::Key ? FrameMeta_FrameType_Key : FrameMeta_FrameType_Delta);
    meta.set_generation_delay(0);

    boost::shared_ptr<Data> d = boost::make_shared<Data>(Name(frameName).append(NameComponents::NameComponentMeta));

    if (settings_.memCache_)
    {
        vector<boost::shared_ptr<const MemoryContentCache::PendingInterest>> pis;
        settings_.memCache_->getPendingInterestsForName(d->getName(), pis);

        if (pis.size())
        {
            uint64_t pitTs = pis[0]->getTimeoutPeriodStart();
            meta.set_generation_delay(nowMs - pitTs);

            LogTraceC << "PIT hit " << pis[0]->getInterest()->toUri() << endl;
        }
    }

    d->getMetaInfo().setFreshnessPeriod(ft == FrameType::Key ? freshness_.keySample_ : freshness_.sample_);
    string metaPayload = meta.SerializeAsString();;

    ndntools::ContentMetaInfo metaInfo;
    metaInfo.setContentType(NDNRTC_FRAME_TYPE)
            .setTimestamp(nowMs)
            .setHasSegments(true)
            .setOther(Blob::fromRawStr(metaPayload));

    d->setContent(metaInfo.wireEncode());
    sign(*d);

    return d;
}

Name
VideoStreamImpl2::publishGop(const Name &framePrefix, const Name &prevFramePrefix,
                             vector<boost::shared_ptr<Data>> &packets)
{
    Name gopPrefix = Name(framePrefix.getPrefix(-1)).append(NameComponents::Gop);

    // check if need to publish end pointer
    if (gopSeq_ > 0)
    {
        Name endGopName = Name(gopPrefix)
                            .appendSequenceNumber(gopSeq_)
                            .append(NameComponents::GopEnd);
        DelegationSet set;
        set.add(0, prevFramePrefix);

        boost::shared_ptr<Data> d = boost::make_shared<Data>(endGopName);
        d->getMetaInfo().setFreshnessPeriod(freshness_.gop_);
        d->setContent(set.wireEncode());
        sign(*d);
        packets.push_back(d);

        LogDebugC << "● end gop " << d->getName() << " -> " << set.get(0).getName() << endl;
    }

    Name startGopName = Name(gopPrefix)
                        .appendSequenceNumber(gopSeq_+1)
                        .append(NameComponents::GopStart);
    DelegationSet set;
    set.add(0, framePrefix);

    boost::shared_ptr<Data> d = boost::make_shared<Data>(startGopName);
    d->getMetaInfo().setFreshnessPeriod(freshness_.gop_);
    d->setContent(set.wireEncode());
    sign(*d);
    packets.push_back(d);

    LogDebugC << "○ start gop " << d->getName() << " -> " << set.get(0).getName() << endl;

    return gopPrefix.appendSequenceNumber(gopSeq_);
}

void
VideoStreamImpl2::sign(Data& d, bool phony)
{
    if (phony)
    {
        static uint8_t digest[ndn_SHA256_DIGEST_SIZE];
        memset(digest, 0, ndn_SHA256_DIGEST_SIZE);
        ndn::Blob signatureBits(digest, sizeof(digest));
        d.setSignature(ndn::DigestSha256Signature());
        ndn::DigestSha256Signature *sha256Signature = (ndn::DigestSha256Signature *)d.getSignature();
        sha256Signature->setSignature(signatureBits);
    }
    else
    {
        keyChain_->sign(d);
        (*statStorage_)[Indicator::SignNum]++;
    }

    (*statStorage_)[Indicator::BytesPublished] += d.getContent().size();
    (*statStorage_)[Indicator::RawBytesPublished] += d.getDefaultWireEncoding().size();
    (*statStorage_)[Indicator::PublishedSegmentsNum]++;
}

void
VideoStreamImpl2::onLiveMetadataRequest(const boost::shared_ptr<const Name>& prefix,
                                    const boost::shared_ptr<const Interest>& interest, Face& face,
                                    uint64_t interestFilterId,
                                    const boost::shared_ptr<const InterestFilter>& filter)
{
    boost::shared_ptr<Data> d = generateLivePacket();
    face.putData(*d);

    {
        lock_guard<mutex> g(internalMutex_);
        queued_.push_back(d);
    }

    LogDebugC << "_live request satisfied: " << d->getName() << endl;
    // TODO: add _live request Stat
}

void
VideoStreamImpl2::onLatestMetaRequest(const boost::shared_ptr<const Name>& prefix,
                                      const boost::shared_ptr<const Interest>& interest, Face& face,
                                      uint64_t interestFilterId,
                                      const boost::shared_ptr<const InterestFilter>& filter)
{
    boost::shared_ptr<Data> d = generateLatestPacket();
    face.putData(*d);

    {
        lock_guard<mutex> g(internalMutex_);
        queued_.push_back(d);
    }

    LogDebugC << "_latest request satisfied: " << d->getName() << endl;
    (*statStorage_)[Indicator::RdrPointerNum]++;
}


boost::shared_ptr<Data>
VideoStreamImpl2::generateLatestPacket()
{
    Name packetName = Name(streamPrefix_)
                        .append(NameComponents::Latest)
                        .appendTimestamp(lastPublishEpochMs_);
    boost::shared_ptr<Data> d = boost::make_shared<Data>(packetName);
    d->getMetaInfo().setFreshnessPeriod(freshness_.latest_);

    DelegationSet set;
    set.add(0, lastFramePrefix_);
    set.add(1, lastGopPrefix_);

    d->setContent(set.wireEncode());
    sign(*d);

    return d;
}

boost::shared_ptr<Data>
VideoStreamImpl2::generateLivePacket()
{
    int64_t seconds = lastCycleMonotonicNs_ / 1E9;
    int32_t nanosec = lastCycleMonotonicNs_ - seconds * 1E9;

    google::protobuf::Timestamp *timestamp = new google::protobuf::Timestamp();
    timestamp->set_seconds(seconds);
    timestamp->set_nanos(nanosec);

    LiveMeta liveMeta;
    liveMeta.set_allocated_timestamp(timestamp);
    liveMeta.set_framerate(liveMetadata_.getRate());
    liveMeta.set_segnum_delta(liveMetadata_.getSegmentsNumEstimate(FrameType::Delta, SegmentClass::Data));
    liveMeta.set_segnum_delta_parity(liveMetadata_.getSegmentsNumEstimate(FrameType::Delta, SegmentClass::Parity));
    liveMeta.set_segnum_key(liveMetadata_.getSegmentsNumEstimate(FrameType::Key, SegmentClass::Data));
    liveMeta.set_segnum_key_parity(liveMetadata_.getSegmentsNumEstimate(FrameType::Key, SegmentClass::Parity));

    Name packetName = Name(streamPrefix_)
                        .append(NameComponents::Live)
                        .appendTimestamp(lastPublishEpochMs_);
    boost::shared_ptr<Data> d = boost::make_shared<Data>(packetName);
    d->getMetaInfo().setFreshnessPeriod(freshness_.live_);
    d->setContent(Blob::fromRawStr(liveMeta.SerializeAsString()));

    sign(*d);
    return d;
}
