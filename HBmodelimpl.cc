#include "HBmodelimpl.h"
#include "HBbasemodelimpl.h"
#include "HBqueuemanager.h"

#include "emane/configureexception.h"
#include "emane/controls/frequencyofinterestcontrolmessage.h"
#include "emane/controls/flowcontrolcontrolmessage.h"
#include "emane/controls/serializedcontrolmessage.h"
#include "emane/mactypes.h"

#include "emane/controls/frequencycontrolmessage.h"
#include "emane/controls/frequencycontrolmessageformatter.h"
#include "emane/controls/receivepropertiescontrolmessage.h"
#include "emane/controls/receivepropertiescontrolmessageformatter.h"
#include "emane/controls/timestampcontrolmessage.h"
#include "emane/controls/transmittercontrolmessage.h"

#include "emane/models/tdma/txslotinfosformatter.h"
#include "HBbasemodelmessage.h"
#include "emane/models/tdma/priority.h"



namespace
{
  const std::string QUEUMANAGER_PREFIX{"queue."};
  const std::string SCHEDULER_PREFIX{"scheduler."};
}


EMANE::Models::HeavyBall::HBmodel::Implementation::
Implementation(NEMId id,
               PlatformServiceProvider *pPlatformServiceProvider,
               RadioServiceProvider * pRadioServiceProvider,
               Scheduler * pScheduler,
               QueueManager * pQueueManager,
               HeavyBallShimLayerImplementor * pRadioModel):
  HeavyBallShimLayerImplementor{id,pPlatformServiceProvider,pRadioServiceProvider},
  pScheduler_{pScheduler},
  pQueueManager_{pQueueManager},
  pRadioModel_{pRadioModel},
  bFlowControlEnable_{},
  u16FlowControlTokens_{},
  sPCRCurveURI_{},
  transmitTimedEventId_{},
  nextMultiFrameTime_{},
  txSlotInfos_{},
  slotDuration_{},
  slotOverhead_{},
  u64SequenceNumber_{},
  frequencies_{},
  u64BandwidthHz_{},
  packetStatusPublisher_{},
  neighborMetricManager_{id},
  receiveManager_{id,
      pRadioModel,
      &pPlatformServiceProvider->logService(),
      pRadioServiceProvider,
      pScheduler,
      &packetStatusPublisher_,
      &neighborMetricManager_},
  flowControlManager_{*pRadioModel},
  u64ScheduleIndex_{}{}


EMANE::Models::HeavyBall::HBmodel::Implementation::~Implementation()
{}


void 
EMANE::Models::HeavyBall::HBmodel::Implementation::initialize(Registrar & registrar)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s",
                          id_,
                          __func__);

  auto & configRegistrar = registrar.configurationRegistrar();

  configRegistrar.registerNumeric<bool>("enablepromiscuousmode",
                                        ConfigurationProperties::DEFAULT |
                                        ConfigurationProperties::MODIFIABLE,
                                        {false},
                                        "Defines whether promiscuous mode is enabled or not."
                                        " If promiscuous mode is enabled, all received packets"
                                        " (intended for the given node or not) that pass the"
                                        " probability of reception check are sent upstream to"
                                        " the transport.");


  configRegistrar.registerNumeric<bool>("flowcontrolenable",
                                        ConfigurationProperties::DEFAULT,
                                        {false},
                                        "Defines whether flow control is enabled. Flow control only works"
                                        " with the virtual transport and the setting must match the setting"
                                        " within the virtual transport configuration.");

  configRegistrar.registerNumeric<std::uint16_t>("flowcontroltokens",
                                                 ConfigurationProperties::DEFAULT,
                                                 {10},
                                                 "Defines the maximum number of flow control tokens"
                                                 " (packet transmission units) that can be processed from the"
                                                 " virtual transport without being refreshed. The number of"
                                                 " available tokens at any given time is coordinated with the"
                                                 " virtual transport and when the token count reaches zero, no"
                                                 " further packets are transmitted causing application socket"
                                                 " queues to backup.");

  configRegistrar.registerNonNumeric<std::string>("pcrcurveuri",
                                                  ConfigurationProperties::REQUIRED,
                                                  {},
                                                  "Defines the URI of the Packet Completion Rate (PCR) curve"
                                                  " file. The PCR curve file contains probability of reception curves"
                                                  " as a function of Signal to Interference plus Noise Ratio (SINR).");


  configRegistrar.registerNumeric<std::uint16_t>("fragmentcheckthreshold",
                                                 ConfigurationProperties::DEFAULT,
                                                 {2},
                                                 "Defines the rate in seconds a check is performed to see if any packet"
                                                 " fragment reassembly efforts should be abandoned.");

  configRegistrar.registerNumeric<std::uint16_t>("fragmenttimeoutthreshold",
                                                 ConfigurationProperties::DEFAULT,
                                                 {5},
                                                 "Defines the threshold in seconds to wait for another packet fragment"
                                                 " for an existing reassembly effort before abandoning the effort.");

  configRegistrar.registerNumeric<float>("neighbormetricdeletetime",
                                         ConfigurationProperties::DEFAULT |
                                         ConfigurationProperties::MODIFIABLE,
                                         {60.0f},
                                         "Defines the time in seconds of no RF receptions from a given neighbor"
                                         " before it is removed from the neighbor table.",
                                         1.0f,
                                         3660.0f);


  configRegistrar.registerNumeric<float>("neighbormetricupdateinterval",
                                         ConfigurationProperties::DEFAULT,
                                         {1.0f},
                                         "Defines the neighbor table update interval in seconds.",
                                         0.1f,
                                         60.0f);

  auto & statisticRegistrar = registrar.statisticRegistrar();

  packetStatusPublisher_.registerStatistics(statisticRegistrar);

  slotStatusTablePublisher_.registerStatistics(statisticRegistrar);

  neighborMetricManager_.registerStatistics(statisticRegistrar);

  aggregationStatusPublisher_.registerStatistics(statisticRegistrar);

  pQueueManager_->setPacketStatusPublisher(&packetStatusPublisher_);

  pQueueManager_->initialize(registrar);

  pScheduler_->initialize(registrar);
}


void
EMANE::Models::HeavyBall::HBmodel::Implementation::configuration(const ConfigurationUpdate & update)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s",
                          id_,
                          __func__);

  ConfigurationUpdate schedulerConfiguration{};
  ConfigurationUpdate queueManagerConfiguration{};

  for(const auto & item : update)
    {
      if(item.first == "enablepromiscuousmode")
        {
          bool bPromiscuousMode{item.second[0].asBool()};

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu HeavyBall::HBmodel::%s: %s = %s",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  bPromiscuousMode ? "on" : "off");

          receiveManager_.setPromiscuousMode(bPromiscuousMode);
        }
      else if(item.first == "flowcontrolenable")
        {
          bFlowControlEnable_ = item.second[0].asBool();

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu HeavyBall::HBmodel::%s: %s = %s",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  bFlowControlEnable_ ? "on" : "off");
        }
      else if(item.first == "flowcontroltokens")
        {
          u16FlowControlTokens_ = item.second[0].asUINT16();

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu HeavyBall::HBmodel::%s: %s = %hu",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  u16FlowControlTokens_);
        }
      else if(item.first == "pcrcurveuri")
        {
          sPCRCurveURI_ = item.second[0].asString();

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu HeavyBall::HBmodel::%s: %s = %s",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  sPCRCurveURI_.c_str());

          receiveManager_.loadCurves(sPCRCurveURI_);
        }
      else if(item.first == "fragmentcheckthreshold")
        {
          std::chrono::seconds fragmentCheckThreshold{item.second[0].asUINT16()};

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu HeavyBall::HBmodel::%s: %s = %lu",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  fragmentCheckThreshold.count());

          receiveManager_.setFragmentCheckThreshold(fragmentCheckThreshold);
        }
      else if(item.first == "fragmenttimeoutthreshold")
        {
          std::chrono::seconds fragmentTimeoutThreshold{item.second[0].asUINT16()};

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu HeavyBall::HBmodel::%s: %s = %lu",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  fragmentTimeoutThreshold.count());

          receiveManager_.setFragmentTimeoutThreshold(fragmentTimeoutThreshold);
        }
      else if(item.first == "neighbormetricdeletetime")
        {
          Microseconds neighborMetricDeleteTimeMicroseconds =
            std::chrono::duration_cast<Microseconds>(DoubleSeconds{item.second[0].asFloat()});

          // set the neighbor delete time
          neighborMetricManager_.setNeighborDeleteTimeMicroseconds(neighborMetricDeleteTimeMicroseconds);

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  DEBUG_LEVEL,
                                  "MACI %03hu HeavyBall::HBmodel::%s %s = %lf",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  std::chrono::duration_cast<DoubleSeconds>(neighborMetricDeleteTimeMicroseconds).count());
        }
      else if(item.first == "neighbormetricupdateinterval")
        {
          neighborMetricUpdateInterval_ =
            std::chrono::duration_cast<Microseconds>(DoubleSeconds{item.second[0].asFloat()});

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu HeavyBall::HBmodel::%s %s = %lf",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  std::chrono::duration_cast<DoubleSeconds>(neighborMetricUpdateInterval_).count());
        }
      else
        {
          if(!item.first.compare(0,SCHEDULER_PREFIX.size(),SCHEDULER_PREFIX))
            {
              schedulerConfiguration.push_back(item);
            }
          else if(!item.first.compare(0,QUEUEMANAGER_PREFIX.size(),QUEUEMANAGER_PREFIX))
            {
              queueManagerConfiguration.push_back(item);
            }
          else
            {
              throw makeException<ConfigureException>("HeavyBall::HBmodel: "
                                                      "Ambiguous configuration item %s.",
                                                      item.first.c_str());
            }
        }
    }

  pQueueManager_->configure(queueManagerConfiguration);

  pScheduler_->configure(schedulerConfiguration);
}

void 
EMANE::Models::HeavyBall::HBmodel::Implementation::start()
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s",
                          id_,
                          __func__);

  pQueueManager_->start();

  pScheduler_->start();
}


void
EMANE::Models::HeavyBall::HBmodel::Implementation::postStart()
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s",
                          id_,
                          __func__);

  pQueueManager_->postStart();

  pScheduler_->postStart();

  // check flow control enabled
  if(bFlowControlEnable_)
    {
      // start flow control
      flowControlManager_.start(u16FlowControlTokens_);

      LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                              DEBUG_LEVEL,
                              "MACI %03hu HeavyBall::HBmodel::%s sent a flow control token update,"
                              " a handshake response is required to process packets",
                              id_,
                              __func__);
    }

  pPlatformService_->timerService().
    schedule(std::bind(&NeighborMetricManager::updateNeighborStatus,
                       &neighborMetricManager_),
             Clock::now() + neighborMetricUpdateInterval_,
             neighborMetricUpdateInterval_);
}


void
EMANE::Models::HeavyBall::HBmodel::Implementation::stop()
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s",
                          id_,
                          __func__);

  // check flow control enabled
  if(bFlowControlEnable_)
    {
      // stop the flow control manager
      flowControlManager_.stop();
    }

  pQueueManager_->stop();

  pScheduler_->stop();
}


void
EMANE::Models::HeavyBall:HBmodel::Implementation::destroy()
  throw()
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s",
                          id_,
                          __func__);

  pQueueManager_->destroy();

  pScheduler_->destroy();
}


void
EMANE::Models::HeavyBall::HBmodel::Implementation::processUpstreamControl(const ControlMessages &)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s",
                          id_,
                          __func__);

}


//TODO::Check if it covers all the things we need to demonstrate.

void
EMANE::Models::HeavyBall::HBmodel::Implementation::processUpstreamPacket(const CommonMACHeader & header,
                                                                         UpstreamPacket & pkt,
                                                                         const ControlMessages & msgs)
{
  auto now = Clock::now();

  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s",
                          id_,
                          __func__);


  const PacketInfo & pktInfo{pkt.getPacketInfo()};

  if(hdr.getRegistrationId() != REGISTERED_EMANE_MAC_TDMA)
    {
      LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                              ERROR_LEVEL,
                              "MACI %03hu HeavyBall::HBmodel::%s: HeavyBall Registration Id %hu does not match our Id %hu, drop.",
                              id_,
                              __func__,
                              hdr.getRegistrationId(),
                              REGISTERED_EMANE_MAC_TDMA);


      packetStatusPublisher_.inbound(pktInfo.getSource(),
                                     pktInfo.getSource(),
                                     pktInfo.getPriority(),
                                     pkt.length(),
                                     PacketStatusPublisher::InboundAction::DROP_REGISTRATION_ID);

      // drop
      return;
    }

  size_t len{pkt.stripLengthPrefixFraming()};

  if(len && pkt.length() >= len)
    {
      BaseModelMessage baseModelMessage{pkt.get(), len};


      const Controls::ReceivePropertiesControlMessage * pReceivePropertiesControlMessage{};

      const Controls::FrequencyControlMessage * pFrequencyControlMessage{};

      for(auto & pControlMessage : msgs)
        {
          switch(pControlMessage->getId())
            {
            case EMANE::Controls::ReceivePropertiesControlMessage::IDENTIFIER:
              {
                pReceivePropertiesControlMessage =
                  static_cast<const Controls::ReceivePropertiesControlMessage *>(pControlMessage);

                LOGGER_VERBOSE_LOGGING_FN_VARGS(pPlatformService_->logService(),
                                                DEBUG_LEVEL,
                                                Controls::ReceivePropertiesControlMessageFormatter(pReceivePropertiesControlMessage),
                                                "MACI %03hu HeavyBall::HBmodel::%s Receiver Properties Control Message",
                                                id_,
                                                __func__);
              }
              break;

            case Controls::FrequencyControlMessage::IDENTIFIER:
              {
                pFrequencyControlMessage =
                  static_cast<const Controls::FrequencyControlMessage *>(pControlMessage);

                LOGGER_VERBOSE_LOGGING_FN_VARGS(pPlatformService_->logService(),
                                                DEBUG_LEVEL,
                                                Controls::FrequencyControlMessageFormatter(pFrequencyControlMessage),
                                                "MACI %03hu HeavyBall::HBmodel::%s Frequency Control Message",
                                                id_,
                                                __func__);

              }

              break;
            }
        }

      if(!pReceivePropertiesControlMessage || !pFrequencyControlMessage ||
         pFrequencyControlMessage->getFrequencySegments().empty())
        {
          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  ERROR_LEVEL,
                                  "MACI %03hu HeavyBall::HBmodel::%s: phy control "
                                  "message not provided from src %hu, drop",
                                  id_,
                                  __func__,
                                  pktInfo.getSource());

          packetStatusPublisher_.inbound(pktInfo.getSource(),
                                         baseModelMessage.getMessages(),
                                         PacketStatusPublisher::InboundAction::DROP_BAD_CONTROL);

          // drop
          return;
        }

      const auto & frequencySegments = pFrequencyControlMessage->getFrequencySegments();

      const FrequencySegment & frequencySegment{*frequencySegments.begin()};

      TimePoint startOfReception{pReceivePropertiesControlMessage->getTxTime() +
          pReceivePropertiesControlMessage->getPropagationDelay() +
          frequencySegment.getOffset()};


      // if EOR slot does not match the SOT slot drop the packet
      auto eorSlotInfo = pScheduler_->getSlotInfo(startOfReception +
                                                  frequencySegment.getDuration());

      // message is too long for slot
      if(eorSlotInfo.u64AbsoluteSlotIndex_ != baseModelMessage.getAbsoluteSlotIndex())
        {
          // determine current slot based on now time to update rx slot status table
          auto slotInfo = pScheduler_->getSlotInfo(now);

          Microseconds timeRemainingInSlot{};

          // ratio calcualtion for slot status tables
          if(slotInfo.u64AbsoluteSlotIndex_ == baseModelMessage.getAbsoluteSlotIndex())
            {
              timeRemainingInSlot = slotDuration_ -
                std::chrono::duration_cast<Microseconds>(now -
                                                         slotInfo.timePoint_);
            }
          else
            {
              timeRemainingInSlot = slotDuration_ +
                std::chrono::duration_cast<Microseconds>(now -
                                                         slotInfo.timePoint_);
            }

          double dSlotRemainingRatio =
            timeRemainingInSlot.count() / static_cast<double>(slotDuration_.count());

          slotStatusTablePublisher_.update(slotInfo.u32RelativeIndex_,
                                           slotInfo.u32RelativeFrameIndex_,
                                           slotInfo.u32RelativeSlotIndex_,
                                           SlotStatusTablePublisher::Status::RX_TOOLONG,
                                           dSlotRemainingRatio);

          packetStatusPublisher_.inbound(pktInfo.getSource(),
                                         baseModelMessage.getMessages(),
                                         PacketStatusPublisher::InboundAction::DROP_TOO_LONG);


          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  DEBUG_LEVEL,
                                  "MACI %03hu HeavyBall::HBmodel::%s eor rx slot:"
                                  " %zu does not match sot slot: %zu, drop long",
                                  id_,
                                  __func__,
                                  eorSlotInfo.u64AbsoluteSlotIndex_,
                                  baseModelMessage.getAbsoluteSlotIndex());

          // drop
          return;
        }

      // rx slot info for now
      auto entry = pScheduler_->getRxSlotInfo(now);

      if(entry.first.u64AbsoluteSlotIndex_ == baseModelMessage.getAbsoluteSlotIndex())
        {
          Microseconds timeRemainingInSlot{slotDuration_ -
              std::chrono::duration_cast<Microseconds>(now -
                                                       entry.first.timePoint_)};

          double dSlotRemainingRatio =
            timeRemainingInSlot.count() / static_cast<double>(slotDuration_.count());

          if(entry.second)
            {
              if(entry.first.u64FrequencyHz_ == frequencySegment.getFrequencyHz())
                {
                  // we are in an RX Slot
                  slotStatusTablePublisher_.update(entry.first.u32RelativeIndex_,
                                                   entry.first.u32RelativeFrameIndex_,
                                                   entry.first.u32RelativeSlotIndex_,
                                                   SlotStatusTablePublisher::Status::RX_GOOD,
                                                   dSlotRemainingRatio);

                  Microseconds span{pReceivePropertiesControlMessage->getSpan()};

                  if(receiveManager_.enqueue(std::move(baseModelMessage),
                                             pktInfo,
                                             pkt.length(),
                                             startOfReception,
                                             frequencySegments,
                                             span,
                                             now,
                                             hdr.getSequenceNumber()))
                    {
                      pPlatformService_->timerService().
                        schedule(std::bind(&ReceiveManager::process,
                                           &receiveManager_,
                                           entry.first.u64AbsoluteSlotIndex_+1),
                                 entry.first.timePoint_+ slotDuration_);
                    }
                }
              else
                {
                  slotStatusTablePublisher_.update(entry.first.u32RelativeIndex_,
                                                   entry.first.u32RelativeFrameIndex_,
                                                   entry.first.u32RelativeSlotIndex_,
                                                   SlotStatusTablePublisher::Status::RX_WRONGFREQ,
                                                   dSlotRemainingRatio);

                  packetStatusPublisher_.inbound(pktInfo.getSource(),
                                                 baseModelMessage.getMessages(),
                                                 PacketStatusPublisher::InboundAction::DROP_FREQUENCY);

                  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                          DEBUG_LEVEL,
                                          "MACI %03hu HeavyBall::HBmodel::%s drop reason rx slot correct"
                                          " rframe: %u rslot: %u but frequency mismatch expected: %zu got: %zu",
                                          id_,
                                          __func__,
                                          entry.first.u32RelativeFrameIndex_,
                                          entry.first.u32RelativeSlotIndex_,
                                          entry.first.u64FrequencyHz_,
                                          frequencySegment.getFrequencyHz());

                  // drop
                  return;
                }
            }
          else
            {
              // not an rx slot but it is the correct abs slot
              auto slotInfo = pScheduler_->getSlotInfo(entry.first.u64AbsoluteSlotIndex_);

              slotStatusTablePublisher_.update(entry.first.u32RelativeIndex_,
                                               entry.first.u32RelativeFrameIndex_,
                                               entry.first.u32RelativeSlotIndex_,
                                               slotInfo.type_ == SlotInfo::Type::IDLE ?
                                               SlotStatusTablePublisher::Status::RX_IDLE :
                                               SlotStatusTablePublisher::Status::RX_TX,
                                               dSlotRemainingRatio);

              packetStatusPublisher_.inbound(pktInfo.getSource(),
                                             baseModelMessage.getMessages(),
                                             PacketStatusPublisher::InboundAction::DROP_SLOT_NOT_RX);


              LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                      DEBUG_LEVEL,
                                      "MACI %03hu HeavyBall::HBmodel::%s drop reason rx slot correct but %s rframe: %u rslot: %u",
                                      id_,
                                      __func__,
                                      slotInfo.type_ == SlotInfo::Type::IDLE ?
                                      "in idle" : "in tx",
                                      entry.first.u32RelativeFrameIndex_,
                                      entry.first.u32RelativeSlotIndex_);


              // drop
              return;
            }
        }
      else
        {
          auto slotInfo = pScheduler_->getSlotInfo(entry.first.u64AbsoluteSlotIndex_);

          Microseconds timeRemainingInSlot{slotDuration_ +
              std::chrono::duration_cast<Microseconds>(now -
                                                       slotInfo.timePoint_)};
          double dSlotRemainingRatio =
            timeRemainingInSlot.count() / static_cast<double>(slotDuration_.count());


          // were we supposed to be in rx on the pkt abs slot
          if(slotInfo.type_ == SlotInfo::Type::RX)
            {
              slotStatusTablePublisher_.update(entry.first.u32RelativeIndex_,
                                               entry.first.u32RelativeFrameIndex_,
                                               entry.first.u32RelativeSlotIndex_,
                                               SlotStatusTablePublisher::Status::RX_MISSED,
                                               dSlotRemainingRatio);

              packetStatusPublisher_.inbound(pktInfo.getSource(),
                                             baseModelMessage.getMessages(),
                                             PacketStatusPublisher::InboundAction::DROP_SLOT_MISSED_RX);

              LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                      DEBUG_LEVEL,
                                      "MACI %03hu TDMA::BaseModel::%s drop reason slot mismatch pkt: %zu now: %zu ",
                                      id_,
                                      __func__,
                                      baseModelMessage.getAbsoluteSlotIndex(),
                                      entry.first.u64AbsoluteSlotIndex_);

              // drop
              return;
            }
          else
            {
              slotStatusTablePublisher_.update(entry.first.u32RelativeIndex_,
                                               entry.first.u32RelativeFrameIndex_,
                                               entry.first.u32RelativeSlotIndex_,
                                               slotInfo.type_ == SlotInfo::Type::IDLE ?
                                               SlotStatusTablePublisher::Status::RX_IDLE :
                                               SlotStatusTablePublisher::Status::RX_TX,
                                               dSlotRemainingRatio);

              packetStatusPublisher_.inbound(pktInfo.getSource(),
                                             baseModelMessage.getMessages(),
                                             PacketStatusPublisher::InboundAction::DROP_SLOT_NOT_RX);


              LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                      DEBUG_LEVEL,
                                      "MACI %03hu HeavyBall::HBmodel::%s drop reason slot mismatch but %s pkt: %zu now: %zu ",
                                      id_,
                                      __func__,
                                      slotInfo.type_ == SlotInfo::Type::IDLE ?
                                      "in idle" : "in tx",
                                      baseModelMessage.getAbsoluteSlotIndex(),
                                      entry.first.u64AbsoluteSlotIndex_);

              // drop
              return;
            }
        }
    }
  else
    {
      LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                              ERROR_LEVEL,
                              "MACI %03hu HeavyBall::HBmodel::%s Packet payload length %zu does not match length prefix %zu",
                              id_,
                              __func__,
                              pkt.length(),
                              len);
    }
}



void 
EMANE::Models::HeavyBall::HBmodel::Implementation::processDownstreamControl(const ControlMessages & msgs)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s",
                          id_,
                          __func__);

  for(const auto & pMessage : msgs)
    {
      LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                              DEBUG_LEVEL,
                              "MACI %03hu HeavyBall::HBmodel::%s downstream control message id %hu",
                              id_,
                              __func__,
                              pMessage->getId());

      switch(pMessage->getId())
        {
        case Controls::FlowControlControlMessage::IDENTIFIER:
          {
            const auto pFlowControlControlMessage =
              static_cast<const Controls::FlowControlControlMessage *>(pMessage);

            if(bFlowControlEnable_)
              {
                LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                        DEBUG_LEVEL,
                                        "MACI %03hu HeavyBall::HBmodel::%s received a flow control token request/response",
                                        id_,
                                        __func__);

                flowControlManager_.processFlowControlMessage(pFlowControlControlMessage);
              }
            else
              {
                LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                        ERROR_LEVEL,
                                        "MACI %03hu HeavyBall::HBmodel::%s received a flow control token request but"
                                        " flow control is not enabled",
                                        id_,
                                        __func__);
              }
          }
          break;

        case Controls::SerializedControlMessage::IDENTIFIER:
          {
            const auto pSerializedControlMessage =
              static_cast<const Controls::SerializedControlMessage *>(pMessage);

            switch(pSerializedControlMessage->getSerializedId())
              {
              case Controls::FlowControlControlMessage::IDENTIFIER:
                {
                  std::unique_ptr<Controls::FlowControlControlMessage>
                    pFlowControlControlMessage{
                    Controls::FlowControlControlMessage::create(pSerializedControlMessage->getSerialization())};

                  if(bFlowControlEnable_)
                    {
                      flowControlManager_.processFlowControlMessage(pFlowControlControlMessage.get());
                    }
                  else
                    {
                      LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                              ERROR_LEVEL,
                                              "MACI %03hu HeavyBall::HBmodel::%s received a flow control token request but"
                                              " flow control is not enabled",
                                              id_,
                                              __func__);
                    }
                }
                break;
              }
          }
        }
    }
}


void EMANE::Models::HeavyBall::Implementation::processDownstreamPacket(DownstreamPacket & pkt,
                                                                       const ControlMessages &)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s",
                          id_,
                          __func__);


  // check flow control
  if(bFlowControlEnable_)
    {
      auto status = flowControlManager_.removeToken();

      if(status.second == false)
        {
          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  ERROR_LEVEL,
                                  "MACI %03hu HeavyBall::HBmodel::%s: failed to remove token, drop packet (tokens:%hu)",
                                  id_,
                                  __func__,
                                  status.first);

          const auto & pktInfo = pkt.getPacketInfo();

          packetStatusPublisher_.outbound(pktInfo.getSource(),
                                          pktInfo.getSource(),
                                          pktInfo.getPriority(),
                                          pkt.length(),
                                          PacketStatusPublisher::OutboundAction::DROP_FLOW_CONTROL);

          // drop
          return;
        }
    }

  std::uint8_t u8Queue{priorityToQueue(pkt.getPacketInfo().getPriority())};

  size_t packetsDropped{pQueueManager_->enqueue(u8Queue,std::move(pkt))};

  // drop, replace token
  if(bFlowControlEnable_)
    {
      for(size_t i = 0; i < packetsDropped; ++i)
        {
          auto status = flowControlManager_.addToken();

          if(!status.second)
            {
              LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                      ERROR_LEVEL,
                                      "MACI %03hu HeavyBall::HBmodel:::%s: failed to add token (tokens:%hu)",
                                      id_,
                                      __func__,
                                      status.first);
            }
        }
    }
}



void EMANE::Models::HeavyBall::HBmodel::Implementation::processEvent(const EverntId & eventId,
                                                                     const Serialization & serialization)
{
    LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s",
                          id_,
                          __func__);

  pScheduler_->processEvent(eventId,serialization);
}


void EMANE::Models::HeavyBall::HBmodel::Implementation::processConfiguration(const ConfigurationUpdate & update)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s",
                          id_,
                          __func__);


  ConfigurationUpdate schedulerConfiguration;

  for(const auto & item : update)
    {
      if(item.first == "enablepromiscuousmode")
        {
          bool bPromiscuousMode{item.second[0].asBool()};

          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  INFO_LEVEL,
                                  "MACI %03hu HeavyBall::HBmodel::%s: %s = %s",
                                  id_,
                                  __func__,
                                  item.first.c_str(),
                                  bPromiscuousMode ? "on" : "off");

          receiveManager_.setPromiscuousMode(bPromiscuousMode);
        }
      else
        {
          schedulerConfiguration.push_back(item);
        }
    }

  pScheduler_->configure(schedulerConfiguration);
}


void EMANE::Models::HeavyBall::HBmodel::Implementation::notifyScheduleChange(const Frequencies & frequencies,
                                                                             std::uint64_t U64BandwidthHz,
                                                                             const Microseconds & slotDuration,
                                                                             const Microseconds & slotOverhead
                                                                             float beat)
{
    LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s",
                          id_,
                          __func__);

  // increment index to indicate a schedule change
  ++u64ScheduleIndex_;
  BETA_ = beta;

  if(transmitTimedEventId_)
    {
      pPlatformService_->timerService().cancelTimedEvent(transmitTimedEventId_);
      transmitTimedEventId_ = 0;
    }

  if(u64BandwidthHz_ != u64BandwidthHz || frequencies != frequencies_)
    {
      // only required if freq set/bandwidth differs from existing
      pRadioModel_->sendDownstreamControl({Controls::FrequencyOfInterestControlMessage::create(u64BandwidthHz,frequencies)});

      frequencies_ = frequencies;

      u64BandwidthHz_ = u64BandwidthHz;
    }

  slotDuration_ = slotDuration;

  slotOverhead_ = slotOverhead;

  slotStatusTablePublisher_.clear();

  std::tie(txSlotInfos_,
           nextMultiFrameTime_) =
    pScheduler_->getTxSlotInfo(Clock::now(),1);

  LOGGER_VERBOSE_LOGGING_FN_VARGS(pPlatformService_->logService(),
                                  DEBUG_LEVEL,
                                  TxSlotInfosFormatter(txSlotInfos_),
                                  "MACI %03hu HeavyBall::HBmodel::%s TX Slot Info",
                                  id_,
                                  __func__);



  if(!txSlotInfos_.empty())
    {
      pendingTxSlotInfo_ = *txSlotInfos_.begin();

      txSlotInfos_.pop_front();

      transmitTimedEventId_ =
        pPlatformService_->timerService().
        schedule(std::bind(&Implementation::processTxOpportunity,
                           this,
                           u64ScheduleIndex_),
                 pendingTxSlotInfo_.timePoint_);
    }
}


void EMANE::Models::HeavyBall::HBmodel::Implementation::processSchedulerPacket(DownstreamPacket & pkt)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s",
                          id_,
                          __func__);

  // enqueue into max priority queue
  pQueueManager_->enqueue(4,std::move(pkt));
}



void EMANE::Models::HeavyBall::HBmodel::Implementation::processSchedulerControl(const ControlMessages & msgs)
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s",
                          id_,
                          __func__);

  pRadioModel_->sendDownstreamControl(msgs);
}



EMANE::Models::HeavyBall::QueueInfos EMANE::Models::HeavyBall::Implementation::getPacketQueueInfo() const
{
  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s",
                          id_,
                          __func__);

  return pQueueManager_->getPacketQueueInfo();
}



void EMANE::Models::HeavyBall::HBmodel::Implementation::sendDownstreamPacket(double dSlotRemainingRatio)
{
  // calculate the number of bytes allowed in the slot
  size_t bytesAvailable =
    (slotDuration_.count() - slotOverhead_.count()) / 1000000.0 * pendingTxSlotInfo_.u64DataRatebps_ / 8.0;

  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                          DEBUG_LEVEL,
                          "MACI %03hu HeavyBall::HBmodel::%s current slot dst is %hu",
                          id_,
                          __func__,
                          pendingTxSlotInfo_.destination_);

  NEMId dst = getDstByMaxWeight();

  auto entry = pQueueManager_->dequeue(pendingTxSlotInfo_.u8QueueId_,
                                       bytesAvailable,
                                       pendingTxSlotInfo_.destination_);

  MessageComponents & components = std::get<0>(entry);
  size_t totalSize{std::get<1>(entry)};

  if(totalSize)
    {
      if(totalSize <= bytesAvailable)
        {
          float fSeconds{totalSize * 8.0f / pendingTxSlotInfo_.u64DataRatebps_};

          Microseconds duration{std::chrono::duration_cast<Microseconds>(DoubleSeconds{fSeconds})};

          // rounding error corner case mitigation
          if(duration >= slotDuration_)
            {
              duration = slotDuration_ - Microseconds{1};
            }

          NEMId dst{};
          size_t completedPackets{};

          // determine how many components represent completed packets (no fragments remain) and
          // whether to use a unicast or broadcast nem address
          for(const auto & component : components)
            {
              completedPackets += !component.isMoreFragments();

              // if not set, set a destination
              if(!dst)
                {
                  dst = component.getDestination();
                }
              else if(dst != NEM_BROADCAST_MAC_ADDRESS)
                {
                  // if the destination is not broadcast, check to see if it matches
                  // the destination of the current component - if not, set the NEM
                  // broadcast address as the dst
                  if(dst != component.getDestination())
                    {
                      dst = NEM_BROADCAST_MAC_ADDRESS;
                    }
                }
            }


          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  DEBUG_LEVEL,
                                  "MACI %03hu HeavyBall::HBmodel::%s sending downstream to %03hu components: %zu",
                                  id_,
                                  __func__,
                                  dst,
                                  components.size());


          if(bFlowControlEnable_ && completedPackets)
            {
              auto status = flowControlManager_.addToken(completedPackets);

              if(!status.second)
                {
                  LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                          ERROR_LEVEL,
                                          "MACI %03hu HeavyBall::HBmodel::%s: failed to add token (tokens:%hu)",
                                          id_,
                                          __func__,
                                          status.first);

                }
            }

          aggregationStatusPublisher_.update(components);

          BaseModelMessage baseModelMessage{pendingTxSlotInfo_.u64AbsoluteSlotIndex_,
              pendingTxSlotInfo_.u64DataRatebps_,
              std::move(components)};

          Serialization serialization{baseModelMessage.serialize()};

          auto now = Clock::now();

          DownstreamPacket pkt({id_,dst,0,now},serialization.c_str(),serialization.size());

          pkt.prependLengthPrefixFraming(serialization.size());

          pRadioModel_->sendDownstreamPacket(CommonMACHeader{REGISTERED_EMANE_MAC_TDMA,u64SequenceNumber_++},
                                             pkt,
                                             {Controls::FrequencyControlMessage::create(
                                                                                        u64BandwidthHz_,
                                                                                        {{pendingTxSlotInfo_.u64FrequencyHz_,duration}}),
                                                 Controls::TimeStampControlMessage::create(pendingTxSlotInfo_.timePoint_),
                                                 Controls::TransmitterControlMessage::create({{id_,pendingTxSlotInfo_.dPowerdBm_}})});

          slotStatusTablePublisher_.update(pendingTxSlotInfo_.u32RelativeIndex_,
                                           pendingTxSlotInfo_.u32RelativeFrameIndex_,
                                           pendingTxSlotInfo_.u32RelativeSlotIndex_,
                                           SlotStatusTablePublisher::Status::TX_GOOD,
                                           dSlotRemainingRatio);

          neighborMetricManager_.updateNeighborTxMetric(dst,
                                                        pendingTxSlotInfo_.u64DataRatebps_,
                                                        now);
        }
      else
        {
          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  ERROR_LEVEL,
                                  "MACI %03hu HeavyBall::HBmodel::%s queue dequeue returning %zu bytes than slot has available %zu",
                                  id_,
                                  __func__,
                                  totalSize,
                                  bytesAvailable);
        }
    }
  else
    {
      // nothing to transmit, update the slot table to record how well
      // schedule is being serviced
      slotStatusTablePublisher_.update(pendingTxSlotInfo_.u32RelativeIndex_,
                                       pendingTxSlotInfo_.u32RelativeFrameIndex_,
                                       pendingTxSlotInfo_.u32RelativeSlotIndex_,
                                       SlotStatusTablePublisher::Status::TX_GOOD,
                                       dSlotRemainingRatio);
    }
}

void EMANE::Models::HeavyBall::HBmodel::Implementation::processTxOpportunity(std::uint64_t u64ScheduleIndex)
{
  // check for scheduled timer functor after new schedule, if so disregard
  if(u64ScheduleIndex != u64ScheduleIndex_)
    {
      LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                              ERROR_LEVEL,
                              "MACI %03hu HeavyBall::HBmodel::%s old schedule tx opportunity found"
                              " scheduled index: %zu current index: %zu",
                              id_,
                              __func__,
                              u64ScheduleIndex,
                              u64ScheduleIndex_);
      return;
    }

  auto now = Clock::now();

  auto nowSlotInfo = pScheduler_->getSlotInfo(now);

  Microseconds timeRemainingInSlot{slotDuration_ -
      std::chrono::duration_cast<Microseconds>(now -
                                               pendingTxSlotInfo_.timePoint_)};
  double dSlotRemainingRatio =
    timeRemainingInSlot.count() / static_cast<double>(slotDuration_.count());

  if(nowSlotInfo.u64AbsoluteSlotIndex_ == pendingTxSlotInfo_.u64AbsoluteSlotIndex_)
    {
      // transmit in this slot
      sendDownstreamPacket(dSlotRemainingRatio);
    }
  else
    {
      slotStatusTablePublisher_.update(pendingTxSlotInfo_.u32RelativeIndex_,
                                       pendingTxSlotInfo_.u32RelativeFrameIndex_,
                                       pendingTxSlotInfo_.u32RelativeSlotIndex_,
                                       SlotStatusTablePublisher::Status::TX_MISSED,
                                       dSlotRemainingRatio);
    }


  // if necessary request more tx opportunities
  if(txSlotInfos_.empty())
    {
      // request more slots
      std::tie(txSlotInfos_,
               nextMultiFrameTime_) =
        pScheduler_->getTxSlotInfo(nextMultiFrameTime_,1);
    }

  bool bFoundTXSlot = {};

  // find the next transmit opportunity
  while(!txSlotInfos_.empty() && !bFoundTXSlot)
    {
      // it might be necessary to request more opportunies if we
      // are behind and have many tx opportunities that have past
      while(!txSlotInfos_.empty())
        {
          pendingTxSlotInfo_ = *txSlotInfos_.begin();

          txSlotInfos_.pop_front();

          if(pendingTxSlotInfo_.u64AbsoluteSlotIndex_ > nowSlotInfo.u64AbsoluteSlotIndex_)
            {
              // need to schedule processing in the future

              transmitTimedEventId_ =
                pPlatformService_->timerService().
                schedule(std::bind(&Implementation::processTxOpportunity,
                                   this,
                                   u64ScheduleIndex_),
                         pendingTxSlotInfo_.timePoint_);

              bFoundTXSlot = true;
              break;
            }
          else if(pendingTxSlotInfo_.u64AbsoluteSlotIndex_ < nowSlotInfo.u64AbsoluteSlotIndex_)
            {
              // missed opportunity
              double dSlotRemainingRatio = \
                std::chrono::duration_cast<Microseconds>(pendingTxSlotInfo_.timePoint_ - now).count() /
                static_cast<double>(slotDuration_.count());

              // blown tx opportunity
              slotStatusTablePublisher_.update(pendingTxSlotInfo_.u32RelativeIndex_,
                                               pendingTxSlotInfo_.u32RelativeFrameIndex_,
                                               pendingTxSlotInfo_.u32RelativeSlotIndex_,
                                               SlotStatusTablePublisher::Status::TX_MISSED,
                                               dSlotRemainingRatio);
            }
          else
            {
              // send the packet
              timeRemainingInSlot = slotDuration_ -
                std::chrono::duration_cast<Microseconds>(now -
                                                         pendingTxSlotInfo_.timePoint_);


              double dSlotRemainingRatio =
                timeRemainingInSlot.count() / static_cast<double>(slotDuration_.count());


              sendDownstreamPacket(dSlotRemainingRatio);
            }
        }

      // if we are out of slots
      if(txSlotInfos_.empty())
        {
          // request more slots
          std::tie(txSlotInfos_,
                   nextMultiFrameTime_) =
            pScheduler_->getTxSlotInfo(nextMultiFrameTime_,1);
        }
    }

  return;
}
//TODO:: Thinking about this implementation maybe we could avoid some things.
EMANE::NEMId EMANE::Models::HeavyBall::HBmodel::Implementation::getDstByMaxWeight()
{
  //TODO:: Yet to be implemented
  
  //TODO:: declare new files to use function getDstQueueLength();
  
  
  auto qls = pQueueManager_->getDstQueueLength();
  
  EMANE::NEMId nemId{0};
  double maxScore = 0;

  if (EMANE::Utils::initialized) 
  {
    counter_++;
    std::string msg = ""; 
    EMANE::Events::Pathlosses pe = EMANE::Utils::pathlossesHolder;
    for (auto const& it: pe)
    {
      if (msg != ""){
        msg.append(",");
      }
      auto id = it.getNEMId();
      auto ql = qls.find(id);
      msg.append(std::to_string(id));
      if (ql == qls.end())
      {
        msg.append(":0");
        continue;
      }

      double weight = lastWeight_[id] + ql->second - lastQueueLength_[id] + BETA_ * (lastWeight_[id] - lastLastWeight_[id]+ ql->second + lastLastQueueLength_[id] - 2 * lastQueueLength_[id]);

      if (weight < 0)
        {
          weight = 0;
        }
      
      weightT_[id] += lastWeight_[id];

      lastLastWeight_[id] = lastWeight_[id];
      lastWeight_[id] = weight;
      lastLastQueueLength_[id] = lastQueueLength_[id];
      lastQueueLength_[id] = ql->second;


      msg.append(":");
      msg.append(std::to_string(weightT_[id] / counter_));

      // todo: change 110 to txpower - noise
      double snr = EMANE::Utils::DB_TO_MILLIWATT(110-it.getForwardPathlossdB());
      // double score = log2(1.0 + snr) * ql->second;
      double score = log2(1.0 + snr) * weight;
      if (score > maxScore)
      {
        nemId = id;
        maxScore = score;
      }
      LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                              DEBUG_LEVEL,
                              "MACI %03hu HeavyBall::HBModel::%s dest: %hu, queuelength: %zu, weight: %f, snr: %f, and score: %f!",
                              id_,
                              __func__,
                              it.getNEMId(),
                              ql->second,
                              weight,
                              snr,
                              score);
    }

    if (counter_ == 1)
     {
        // create socket and ready for send data out.
        counter_ = 0;
        for (int i = 0; i < 10; i++)
          {
            weightT_[i] = 0;
          }
        int sock_fd = -1;
        char buf[MAXDATASIZE];
        int recvbytes, sendbytes, len;

        in_addr_t server_ip = inet_addr("127.0.0.1");
        in_port_t server_port = 10036;


        if ((sock_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  DEBUG_LEVEL,
                                  "MACI %03hu HeavyBall::HBModel::%s Socket creation error!",
                                  id_,
                                  __func__);
        }

        long flag = 1;
        setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, sizeof(flag));

        struct sockaddr_in server_addr;
        server_addr.sin_addr.s_addr = server_ip;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        // printf("Try to connect server(%s:%u)\n", inet_ntoa(server_addr.sin_addr), ntohs(server_addr.sin_port));

        if(connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr)) == -1) {
          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  DEBUG_LEVEL,
                                  "MACI %03hu HeavyBall::BaseModel::%s Connection Failed!",
                                  id_,
                                  __func__);
          close(sock_fd);  
          return nemId;
        }  

        // printf("Connect server success(%s:%u)\n", inet_ntoa(server_addr.sin_addr), ntohs(server_addr.sin_port));
        if(send(sock_fd, msg.c_str(), msg.size(), 0) == -1) {
            LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                        DEBUG_LEVEL,
                        "MACI %03hu HeavyBall::HBModel::%s Send Failed!",
                        id_,
                        __func__);
        }
        if((recvbytes=recv(sock_fd, buf, MAXDATASIZE, 0)) == -1) {  
          LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  DEBUG_LEVEL,
                                  "MACI %03hu HeavyBall::HBModel::%s Connection recv Failed!",
                                  id_,
                                  __func__);
        }
        buf[recvbytes] = '\0';
        LOGGER_STANDARD_LOGGING(pPlatformService_->logService(),
                                  DEBUG_LEVEL,
                                  "MACI %03hu HeavyBall::HBModel::%s \"%s\" recived!",
                                  id_,
                                  __func__,
                                  buf);
        close(sock_fd);  
     }
     
  }

  return nemId;

}
  
  










