#ifndef EMANEMODELSHBRECEIVEMANAGER_HEADER_
#define EMANEMODELSHBRECEIVEMANAGER_HEADER_

#include "emane/types.h"
#include "emane/upstreampacket.h"
#include "emane/downstreamtransport.h"
#include "emane/radioserviceprovider.h"
#include "emane/logserviceprovider.h"
#include "emane/frequencysegment.h"
#include "emane/neighbormetricmanager.h"
#include "emane/models/tdma/scheduler.h"
#include "emane/utils/randomnumberdistribution.h"

#include "pormanager.h"
#include "basemodelmessage.h"
#include "emane/models/tdma/packetstatuspublisher.h"

#include <tuple>

namespace EMANE
{
  namespace Models
  {
    namespace HeavyBall
    {
      /**
       * @class ReceiveManager
       *
       * @brief Manages all receive side message processing
       *
       * Support aggregate message components and fragmentation
       * reassembly. Handles aggregation and fragmentation of inbound
       * messages even when the radio model is configured to not
       * aggregate or fragment transmissions.
       */
      class ReceiveManager
      {
      public:
        ReceiveManager(NEMId id,
                       DownstreamTransport * pDownstreamTransport,
                       LogServiceProvider * pLogService,
                       RadioServiceProvider * pRadioService,
                       Scheduler * pScheduler,
                       PacketStatusPublisher * pPacketStatusPublisher,
                       NeighborMetricManager *pNeighborMetricManager);

        void setFragmentCheckThreshold(const std::chrono::seconds & threshold);

        void setFragmentTimeoutThreshold(const std::chrono::seconds & threshold);

        void setPromiscuousMode(bool bEnable);

        void loadCurves(const std::string & sPCRFileName);

        bool enqueue(BaseModelMessage && baseModelMessage,
                     const PacketInfo & pktInfo,
                     size_t length,
                     const TimePoint & startOfReception,
                     const FrequencySegments & frequencySegments,
                     const Microseconds & span,
                     const TimePoint & beginTime,
                     std::uint64_t u64PacketSequence);

        void process(std::uint64_t u64AbsoluteSlotIndex);

      private:
        NEMId id_;
        DownstreamTransport * pDownstreamTransport_;
        LogServiceProvider * pLogService_;
        RadioServiceProvider * pRadioService_;
        Scheduler * pScheduler_;
        PacketStatusPublisher * pPacketStatusPublisher_;
        NeighborMetricManager * pNeighborMetricManager_;

        using PendingInfo = std::tuple<BaseModelMessage,
                                       PacketInfo,
                                       size_t,
                                       TimePoint, //sor
                                       FrequencySegments,
                                       Microseconds, // span
                                       TimePoint,
                                       std::uint64_t>; // sequence number
        PendingInfo pendingInfo_;
        std::uint64_t u64PendingAbsoluteSlotIndex_;
        PORManager porManager_;
        Utils::RandomNumberDistribution<std::mt19937,
                                        std::uniform_real_distribution<float>> distribution_;

        bool bPromiscuousMode_;
        std::chrono::seconds fragmentCheckThreshold_;
        std::chrono::seconds fragmentTimeoutThreshold_;

        using FragmentKey = std::tuple<NEMId,Priority,std::uint64_t>;
        using FragmentParts = std::map<size_t,std::vector<std::uint8_t>>;
        using FragmentInfo = std::tuple<std::set<size_t>,
                                        FragmentParts,
                                        TimePoint, // last fragment time
                                        NEMId, // destination
                                        Priority,
                                        size_t>; // total number of fragments
        using FragmentStore = std::map<FragmentKey,FragmentInfo>;

        FragmentStore fragmentStore_;
        TimePoint lastFragmentCheckTime_;

        ReceiveManager(const ReceiveManager &) = delete;

        ReceiveManager & operator=(const ReceiveManager &) = delete;
      };
    }
  }
}

#endif
