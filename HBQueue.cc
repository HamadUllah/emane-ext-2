#include "HBQueue.h"

EMANE::Models::HeavyBall::Queue::Queue():
  u16QueueDepth_{},
  bFragment_{},
  u64Counter_{},
  currentBytes_{},
  bIsControl_{}{}


void EMANE::Models::HeavyBall::Queue::initialize(std::uint16_t u16QueueDepth,
                                            bool bFragment,
                                            bool bAggregate,
                                            bool bIsControl)
{
  u16QueueDepth_ = u16QueueDepth;
  bFragment_ = bFragment;
  bIsControl_ = bIsControl;
  bAggregate_ = bAggregate;
}

std::pair<std::unique_ptr<EMANE::DownstreamPacket>,bool>
  EMANE::Models::HeavyBall::Queue::enqueue(DownstreamPacket && pkt)
{
  std::unique_ptr<DownstreamPacket> pDroppedPacket{};
  bool bDroppedPacket{};

  DownstreamPacket * pPkt{new DownstreamPacket{std::move(pkt)}};

  NEMId dest{pPkt->getPacketInfo().getDestination()};

  if(queue_.size() == u16QueueDepth_)
    {
      // first candidate for overflow discard, oldest packet
      auto entry = queue_.begin();

      // search the queue for a packet that is not in process of
      // fragmentation, if all packets are undergoing fragmentation
      // the oldest packet will be discarded
      for(auto iter = queue_.begin();
          iter != queue_.end();
          ++iter)
        {
          // a packet undergoing fragmentation will have a non-zero
          // fragment index
          if(!iter->second.second->index_)
            {
              entry = iter;
              break;
            }
        }

      // ownership transfer
      pDroppedPacket.reset(entry->second.first);

      // ownership transfer
      std::unique_ptr<MetaInfo> pDroppedMetaInfo{entry->second.second};

      bDroppedPacket = true;

      auto dst = pDroppedPacket->getPacketInfo().getDestination();

      destQueue_[dst].erase(entry->first);

      queue_.erase(entry->first);

      // update current bytes in queue by subtracting off amount
      // remaining in dropped packet
      currentBytes_ -= pDroppedPacket->length() - pDroppedMetaInfo->offset_;
    }

  MetaInfo * pMetaInfo = new MetaInfo;

  queue_.insert(std::make_pair(u64Counter_,std::make_pair(pPkt,pMetaInfo)));

  auto iter = destQueue_.find(dest);

  if(iter == destQueue_.end())
    {
      iter = destQueue_.insert({dest,PacketQueue{}}).first;
    }

  currentBytes_ += pPkt->length();

  iter->second.insert(std::make_pair(u64Counter_,std::make_pair(pPkt,pMetaInfo)));

  ++u64Counter_;

  return {std::move(pDroppedPacket),bDroppedPacket};
}

std::tuple<EMANE::Models::HeavyBall::MessageComponents,
           size_t,
           std::list<std::unique_ptr<EMANE::DownstreamPacket>>>
  EMANE::Models::HeavyBall::Queue::dequeue(size_t requestedBytes, NEMId destination,bool bDrop)
{
  MessageComponents components{};
  size_t totalBytes{};
  std::list<std::unique_ptr<DownstreamPacket>> dropped;

  while(totalBytes <= requestedBytes)
    {
      if(destination)
        {
          auto iter = destQueue_.find(destination);

          // check for a destination queue that is not empty
          if(iter != destQueue_.end() && !iter->second.empty())
            {
              auto const & entry = iter->second.begin();

              auto & pPacket = std::get<0>(entry->second);
              auto & pMetaInfo = std::get<1>(entry->second);

              if(pPacket->length() - pMetaInfo->offset_ <= requestedBytes - totalBytes)
                {
                  if(pMetaInfo->offset_)
                    {
                      auto ret = fragmentPacket(pPacket,
                                                pMetaInfo,
                                                entry->first,
                                                requestedBytes - totalBytes);

                      totalBytes += ret.second;

                      components.push_back(std::move(ret.first));
                    }
                  else
                    {
                      components.push_back({bIsControl_ ?
                            MessageComponent::Type::CONTROL :
                            MessageComponent::Type::DATA,
                            pPacket->getPacketInfo().getDestination(),
                            pPacket->getPacketInfo().getPriority(),
                            pPacket->getVectorIO(),
                            pMetaInfo->index_,
                            pMetaInfo->offset_,
                            entry->first,
                            false});


                      totalBytes += pPacket->length() - pMetaInfo->offset_;
                    }

                  delete pPacket;
                  delete pMetaInfo;

                  // remove from packet queue using queue sequence
                  queue_.erase(entry->first);

                  // remove from destination queue using iterator
                  iter->second.erase(entry);

                  // if aggregation is disabled don't look further
                  if(!bAggregate_)
                    {
                      break;
                    }
                }
              else
                {
                  if(bFragment_)
                    {
                      auto ret = fragmentPacket(pPacket,
                                                pMetaInfo,
                                                entry->first,
                                                requestedBytes - totalBytes);

                      totalBytes += ret.second;

                      components.push_back(std::move(ret.first));

                      break;
                    }
                  else
                    {
                      if(bDrop && components.empty())
                        {
                          // drop packet - too large and fragmentation
                          // is disabled
                          currentBytes_ -= pPacket->length();

                          // transfer ownership to std::unique_ptr
                          dropped.push_back(std::unique_ptr<DownstreamPacket>{pPacket});

                          delete pMetaInfo;

                          // remove from packet queue using queue sequence
                          queue_.erase(entry->first);

                          // remove from destination queue using iterator
                          iter->second.erase(entry);
                        }
                      else
                        {
                          break;
                        }
                    }
                }
            }
          else
            {
              break;
            }
        }
      else if(!queue_.empty())
        {
          auto const & entry = queue_.begin();

          auto & pPacket = std::get<0>(entry->second);
          auto & pMetaInfo = std::get<1>(entry->second);

          NEMId dst{pPacket->getPacketInfo().getDestination()};

          if(pPacket->length() - pMetaInfo->offset_ <= requestedBytes - totalBytes)
            {
              if(pMetaInfo->offset_)
                {
                  auto ret = fragmentPacket(pPacket,
                                            pMetaInfo,
                                            entry->first,
                                            requestedBytes - totalBytes);

                  totalBytes += ret.second;

                  components.push_back(std::move(ret.first));
                }
              else
                {
                  components.push_back({bIsControl_ ?
                        MessageComponent::Type::CONTROL :
                        MessageComponent::Type::DATA,
                        dst,
                        pPacket->getPacketInfo().getPriority(),
                        pPacket->getVectorIO(),
                        pMetaInfo->index_,
                        pMetaInfo->offset_,
                        entry->first,
                        false});

                  totalBytes += pPacket->length() - pMetaInfo->offset_;
                }

              delete pPacket;
              delete pMetaInfo;

              // remove from destination queue using queue sequence
              destQueue_[dst].erase(entry->first);

              // remove for packet queue using iterator
              queue_.erase(entry);

              // if aggregation is disabled don't look further
              if(!bAggregate_)
                {
                  break;
                }
            }
          else
            {
              if(bFragment_)
                {
                  auto ret = fragmentPacket(pPacket,
                                            pMetaInfo,
                                            entry->first,
                                            requestedBytes - totalBytes);

                  totalBytes += ret.second;

                  components.push_back(std::move(ret.first));

                  break;
                }
              else
                {
                  if(bDrop && components.empty())
                    {
                      // drop packet - too large and fragmentation
                      // is disabled
                      currentBytes_ -= pPacket->length();

                      // transfer ownership to std::unique_ptr
                      dropped.push_back(std::unique_ptr<DownstreamPacket>{pPacket});

                      delete pMetaInfo;

                      // remove from destination queue using queue sequence
                      destQueue_[dst].erase(entry->first);

                      // remove for packet queue using iterator
                      queue_.erase(entry);
                    }
                  else
                    {
                      break;
                    }
                }
            }
        }
      else
        {
          break;
        }
    }

  // reduce bytes in queue by the total being returned, dropped bytes
  // already accounted for
  currentBytes_ -= totalBytes;

  return std::make_tuple(components,totalBytes,std::move(dropped));
}

std::pair<EMANE::Models::HeavyBall::MessageComponent,size_t>
EMANE::Models::HeavyBall::Queue::fragmentPacket(DownstreamPacket * pPacket,
                                           MetaInfo * pMetaInfo,
                                           std::uint64_t u64FragmentSequence,
                                           size_t bytes)
{
  size_t totalBytesVisited{};
  size_t totalBytesCopied{};
  Utils::VectorIO vectorIOs{};

  // packet data is stored in an iovec, need to determine
  // which iovec to start with and proceed to advance when necessary
  for(const auto & entry : pPacket->getVectorIO())
    {
      if(totalBytesCopied < bytes)
        {
          if(totalBytesVisited + entry.iov_len < pMetaInfo->offset_)
            {
              totalBytesVisited += entry.iov_len;
            }
          else
            {
              char * pBuf{reinterpret_cast<char *>(entry.iov_base)};

              // where are we in the current entry
              auto offset = pMetaInfo->offset_ - totalBytesVisited;

              // how much of this entry is left
              auto remainder = entry.iov_len - offset;

              // if necessary adjust totalBytesVisited after
              // calculating where in the current entry the fragment
              // begins
              if(totalBytesVisited < pMetaInfo->offset_)
                {
                  totalBytesVisited = pMetaInfo->offset_;
                }

              // clamp the reaminder if there is more remaining than
              // what was request
              size_t amountToCopy{};

              if(remainder > bytes - totalBytesCopied)
                {
                  amountToCopy =  bytes - totalBytesCopied;
                  remainder -= amountToCopy;
                }
              else
                {
                  amountToCopy = remainder;
                }

              vectorIOs.push_back(Utils::make_iovec(pBuf+offset,amountToCopy));

              pMetaInfo->offset_ += amountToCopy;
              totalBytesVisited += amountToCopy;
              totalBytesCopied += amountToCopy;
            }
        }
      else
        {
          break;
        }
    }


  MessageComponent component{bIsControl_ ?
      MessageComponent::Type::CONTROL :
      MessageComponent::Type::DATA,
      pPacket->getPacketInfo().getDestination(),
      pPacket->getPacketInfo().getPriority(),
      vectorIOs,
      pMetaInfo->index_,
      pMetaInfo->offset_ - totalBytesCopied,
      u64FragmentSequence,
      totalBytesVisited != pPacket->length()};

  ++pMetaInfo->index_;

  return {component,totalBytesCopied};
}

// packets, bytes
std::tuple<size_t,size_t> EMANE::Models::HeavyBall::Queue::getStatus() const
{
  return std::make_tuple(queue_.size(),currentBytes_);
}

// get all queue length
std::map<std::uint64_t,size_t> EMANE::Models::HeavyBall::Queue::getDestQueueLength()
{
  std::map<std::uint64_t,size_t> destQueueLength{};
  for (auto it=destQueue_.begin(); it!=destQueue_.end(); ++it) 
  {
    destQueueLength.insert(std::make_pair(it->first,it->second.size()));
  } 
  return destQueueLength;
}
