#include "log.hpp"
#include "opendroneid.h"
#include <SenderInfo.hpp>
#include <fcntl.h> // O_CREAT, O_RDONLY
#include <mqueue.h>
#include <string.h>
#include <string>
#include <sys/stat.h> // mode_t
#include <vector>
#include "MsgPacket.h"

static const std::vector<int> ALL_CHANNELS = {
  // 2.4G
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
  // 5G
  36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132,
  136, 140, 144, 149, 153, 157, 161, 165
};

class Recver
{
public:
  enum
  {
    DWELL = 0,
    SCHEDULE
  } state
      = DWELL;
  int index = -1;
  std::string iface = "wlan0";
  int expectedDwellTime = 200;
  int scheduleTimeoutTime = 200;
  int scheduleAdvanceTime = 20;
  int scheduleTimeoutCounter = 0;
  int scheduleShouldDeletedTime = 3600 * 1000; // 1h
  int minScheduleInterval = 1000;
  const std::vector<int> *managedChannels;
  SenderInfoMap *sendersInfo;

  Recver (int idx_, int expectedDwellTime_ = 200,
          int scheduleTimeoutTime_ = 200, int minScheduleInterval_ = 1000,
          const std::vector<int> *managedChannels_ = nullptr,
          SenderInfoMap *sendersInfo_ = nullptr)
      : index (idx_), expectedDwellTime (expectedDwellTime_),
        scheduleTimeoutTime (scheduleTimeoutTime_),
        minScheduleInterval (minScheduleInterval_),
        managedChannels (managedChannels_ ? managedChannels_ : &ALL_CHANNELS),
        sendersInfo (sendersInfo_ ? sendersInfo_ : new SenderInfoMap ())
  {
  }

  ~Recver () { delete sendersInfo; }

private:
  int lastDwellTimestep = -1;
  int enterScheduleTimestep = -1;
  int activeChannelIndex = -1;
  int pollingChannelIndex = 0;

  int
  getTimeStepMs (void)
  {
    struct timespec ts;
    clock_gettime (CLOCK_MONOTONIC, &ts);
    return static_cast<int> (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
  }

  int
  getActiveChannelIndex (void)
  {
    return activeChannelIndex;
  }

  int
  getPollingChannelIndex (void)
  {
    return pollingChannelIndex;
  }

  int
  changeToChannel (int newChannelIndex)
  {
    if (newChannelIndex < 0 || newChannelIndex >= managedChannels->size ())
      {
        log_err ("Invalid channel index: " + std::to_string (newChannelIndex));
        return -1;
      }

    activeChannelIndex = newChannelIndex;
    log_info ("Changed to channel: "
              + std::to_string ((*managedChannels)[activeChannelIndex]));
    std::string cmd
        = "iw dev " + iface + " set channel "
          + std::to_string ((*managedChannels)[activeChannelIndex]);
    int ret = system (cmd.c_str ());
    if (ret != 0)
      {
        log_err ("Failed to change channel: " + std::to_string (ret));
        return -1;
      }
    return 0;
  }

  int
  pollToNextChannel (void)
  {
    pollingChannelIndex = (pollingChannelIndex + 1)
                          % static_cast<int> (managedChannels->size ());
    return changeToChannel (pollingChannelIndex);
  }

  int
  getNextScheduleRecvTimestep (SenderInfo &info)
  {
    if (info.nextSendTimestep > 0 && info.minInterval > 0)
      { // 有计划时间且有记录的最小间隔
        while (info.nextSendTimestep <= getTimeStepMs ())
          { // 错过计划时间，重新规划
            info.nextSendTimestep += info.minInterval;
          }
      }
    return info.nextSendTimestep;
  }

  int
  recordSenderInfo (msgPacket_t *packet_)
  {
    std::string senderID (packet_->ID);
    int timestep = packet_->timestep;
    auto it = sendersInfo->find (senderID);
    if (it == sendersInfo->end ())
      { // 首次收到包，初始化发送者信息

        SenderInfo newInfo (senderID, activeChannelIndex, 1, timestep, -1);
        (*sendersInfo)[senderID] = newInfo;
        log_info ("New sender recorded: " + senderID);
        return 0;
      }
    else
      {
        // 已有发送者记录，更新信息并预计发送时间
        SenderInfo &info = it->second;
        int interval = timestep - info.lastSentTimestep;
        if (info.lastSentTimestep >= 0 && interval > 0)
          {
            info.appendInterval (interval);
          }
        else
          {
            log_err ("Non-positive interval for sender " + senderID
                     + ", last: " + std::to_string (info.lastSentTimestep)
                     + ", current: " + std::to_string (timestep));
          }
        info.lastSentTimestep = timestep;
        info.recvedTimes += 1;
        info.scheduleHandling = false;

        // 更新下次发送时间
        int nextSendTimestep = timestep + minScheduleInterval;
        while (nextSendTimestep - getTimeStepMs () < minScheduleInterval)
          {
            nextSendTimestep += minScheduleInterval;
          }
        info.nextSendTimestep = nextSendTimestep;

        log_info ("Updated sender info: " + senderID);
        return info.recvedTimes;
      }
  }

  int
  channelManagementLoop (void)
  {
    pollingChannelIndex = 0;
    state = DWELL;
    changeToChannel (0);

    // 创建消息队列并写入属性
    struct mq_attr attr;
    memset (&attr, 0, sizeof (attr));
    attr.mq_flags = 0;                      // 0 表示阻塞模式
    attr.mq_maxmsg = 100;                   // 队列中最多 10 条消息
    attr.mq_msgsize = sizeof (msgPacket_t); // 单条消息大小
    attr.mq_curmsgs = 0; // 当前消息条数（只读字段，mq_open 时忽略）
    mqd_t mq = mq_open (MSG_QUEUE_NAME, O_CREAT | O_RDONLY, 0666, nullptr);
    if (mq == (mqd_t)-1)
      {
        log_err ("Failed to open message queue");
        ;
        return -1;
      }

    while (true)
      {
        if (state == DWELL)
          {
            // 非轮询频道需要切换回轮询频道
            if (getActiveChannelIndex () != getPollingChannelIndex ())
              {
                changeToChannel (getPollingChannelIndex ());
              }

            if (getTimeStepMs () - lastDwellTimestep < expectedDwellTime)
              { // 驻留时间内监控消息队列是否有数据包信息，如果有则记录
                mq_getattr (mq, &attr);
                if (attr.mq_curmsgs > 0)
                  {
                    msgPacket_t packet;
                    mq_receive (mq, reinterpret_cast<char *> (&packet),
                                sizeof (packet), nullptr);
                    if (recordSenderInfo (&packet) == 0)
                      { // 首次收到包，重置发包时间，争取接收到该发送机的下一次包
                        lastDwellTimestep = getTimeStepMs ();
                      }
                  }
              }
            else
              {
                pollToNextChannel ();
                lastDwellTimestep = getTimeStepMs ();
              }

            // 判定是否符合调度条件
            // 轮询SenderInfoMap，寻找下次调度时间最近的发送者
            int intervalNextMin = std::numeric_limits<int>::max ();
            SenderInfo *scheduledSenderInfo = nullptr;
            std::vector<SenderInfo *> aboutToDelete;
            for (auto &item : *sendersInfo)
              {
                if (!item.second.scheduleHandling)
                  {
                    SenderInfo &info = item.second;

                    // 未收到包时间过长，则记录准备删除
                    if (getTimeStepMs () - info.lastSentTimestep
                        > scheduleShouldDeletedTime)
                      {
                        aboutToDelete.push_back (&info);
                        continue;
                      }

                    int nextTimestep = getNextScheduleRecvTimestep (info);
                    int intervalToNext = nextTimestep - getTimeStepMs ();

                    // 有调度时间，且小于最小调度间隔，则更新调度者信息
                    if (intervalToNext > 0
                        && intervalToNext < scheduleAdvanceTime
                        && intervalToNext < intervalNextMin)
                      {
                        intervalNextMin = intervalToNext;
                        scheduledSenderInfo = &info;
                      }
                  }
              }

            // 没用的SenderInfo该删就删
            for (SenderInfo *info : aboutToDelete)
              {
                sendersInfo->erase (info->id);
              }

            // 如果有符合调度条件的SenderInfo，切换到调度状态
            if (scheduledSenderInfo != nullptr)
              {
                scheduledSenderInfo->scheduleHandling = true;
                if (getActiveChannelIndex () != getPollingChannelIndex ())
                  {
                    changeToChannel (scheduledSenderInfo->channelIndex);
                  }
                state = SCHEDULE;
                enterScheduleTimestep = getTimeStepMs ();
              }
          }
        else if (state == SCHEDULE)
          {
            if (getTimeStepMs () - enterScheduleTimestep < scheduleTimeoutTime)
              { // 调度时间内监控消息队列是否有数据包信息，如果有则记录
                mq_getattr (mq, &attr);
                if (attr.mq_curmsgs > 0)
                  {
                    msgPacket_t packet;
                    mq_receive (mq, reinterpret_cast<char *> (&packet),
                                sizeof (packet), nullptr);
                    recordSenderInfo (&packet);
                    state = DWELL;
                  }
              }
            else
              { // 超时则切换回轮询，记录超时次数
                state = DWELL;
                scheduleTimeoutCounter += 1;
              }
          }
      }
  }
};