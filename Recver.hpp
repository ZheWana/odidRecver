#pragma once
#include "MsgPacket.h"
#include "SenderInfo.hpp"
#include "log.hpp"
#include "opendroneid.h"
#include <algorithm>
#include <atomic>
#include <fcntl.h> // O_CREAT, O_RDONLY
#include <mqueue.h>
#include <string.h>
#include <string>
#include <sys/stat.h> // mode_t
#include <thread>
#include <unistd.h>
#include <vector>

static const std::vector<int> ALL_CHANNELS = {
  // 2.4G
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
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
  };
  std::atomic<int> state{ DWELL };
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

  Recver (const Recver &) = delete;
  Recver &operator= (const Recver &) = delete;
  Recver (Recver &&) = delete;
  Recver &operator= (Recver &&) = delete;

private:
  int lastDwellTimestep = -1;
  int enterScheduleTimestep = -1;
  std::atomic<int> activeChannelIndex{ -1 };
  int pollingChannelIndex = 0;
  pid_t ridCapturePid;

  int
  getTimeStepMs (void) const
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
    log_info ("Changed to channel: " << (*managedChannels)[activeChannelIndex]
                                     << " (index:" << activeChannelIndex
                                     << ")");
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

  // 根据频率(单位 MHz)转换为标准 Wi-Fi 信道号；失败返回 -1
  static int
  wifiFreq2Channel (int freqMHz)
  {
    // 2.4 GHz band: channel 1~13 (2412 + 5*(ch-1)), channel 14 = 2484
    if (freqMHz == 2484)
      {
        return 14;
      }
    if (freqMHz >= 2412 && freqMHz <= 2472)
      {
        int ch = (freqMHz - 2412) / 5 + 1;
        if (2412 + (ch - 1) * 5 == freqMHz && ch >= 1 && ch <= 13)
          {
            return ch;
          }
      }
    // 5 GHz band
    // 频道中心频率: 5000 + 5 * ch
    // e.g., ch 36 -> 5180, ch 40 -> 5200, etc.
    if (freqMHz >= 5000)
      {
        int ch = (freqMHz - 5000) / 5;
        if (5000 + 5 * ch == freqMHz)
          {
            return ch;
          }
      }
    // 无法匹配
    return -1;
  }
  // freq 为中心频率(MHz)，返回在 ALL_CHANNELS 中的 index，找不到返回 -1
  int
  wifiFreq2chIndex (int freq)
  {
    int ch = wifiFreq2Channel (freq);
    if (ch < 0)
      {
        return -1; // 频率无法映射到有效 Wi-Fi 频道
      }
    auto it = std::find (ALL_CHANNELS.begin (), ALL_CHANNELS.end (), ch);
    if (it == ALL_CHANNELS.end ())
      {
        return -1; // 频道不在 ALL_CHANNELS 列表里
      }
    return static_cast<int> (std::distance (ALL_CHANNELS.begin (), it));
  }

  int
  recordSenderInfo (msgPacket_t *packet_)
  {
    auto chIndex = wifiFreq2chIndex (packet_->channel_freq);
    std::string realID = packet_->ID;
    std::string senderID (realID + "-ch" + std::to_string (chIndex));
    int timestep = packet_->timestep;
    auto it = sendersInfo->find (senderID);
    if (it == sendersInfo->end ())
      { // 首次收到包，初始化发送者信息
        if (chIndex == -1)
          return -1;
        SenderInfo newInfo (senderID, chIndex, 1, timestep, -1);
        (*sendersInfo)[senderID] = newInfo;
        log_info ("New sender recorded: " + senderID);
        return 0;
      }
    else
      {
        // 已有发送者记录，更新信息并预计发送时间
        SenderInfo &info = it->second;
        int interval = timestep - info.lastSentTimestep;
        info.lastInterval = interval;
        info.channelIndex = chIndex;
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

        // 输出详细的info
        log_info ("Updated sender info: " << senderID);
        log_info ("id               : " << senderID);
        log_info ("lastSentTimestep : " << info.lastSentTimestep);
        log_info ("lastInterval     : " << info.lastInterval);
        log_info ("recvedTimes      : " << info.recvedTimes);
        log_info ("nextSendTimestep : " << info.nextSendTimestep);
        log_info ("minInterval      : " << info.minInterval);
        log_info ("channelIndex     : " << info.channelIndex);
        return info.recvedTimes;
      }
  }

  int
  start_rid_capture (void)
  {
    ridCapturePid = fork ();
    if (ridCapturePid < 0)
      {
        // fork 失败
        perror ("fork");
        return -1;
      }
    if (ridCapturePid == 0)
      {
        // ========= 子进程 =========
        int fd = open ("/www/track.json", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
          {
            perror ("open /www/track.json");
            _exit (127);
          }

        if (dup2 (fd, STDOUT_FILENO) < 0)
          {
            perror ("dup2");
            _exit (127);
          }
        close (fd);
        int fd_err = open ("/www/rid_capture.err",
                           O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_err >= 0)
          {
            dup2 (fd_err, STDERR_FILENO);
            close (fd_err);
          }

        char *argv[]
            = { (char *)"./rid_capture", (char *)"-w", (char *)"wlan0", NULL };
        execvp ("./rid_capture", argv);
        // 运行到这里说明execvp失败
        fprintf (stderr, "execvp failed: %s\n", strerror (errno));
        _exit (127);
      }
    // ========= 父进程 =========
    printf ("started rid_capture, child ridCapturePid = %d\n", ridCapturePid);
    return ridCapturePid;
  }

  static void
  dumpIDHex (const char *label, const uint8_t *buf, size_t len)
  {
    std::string s;
    char tmp[8];
    for (size_t i = 0; i < len; ++i)
      {
        snprintf (tmp, sizeof (tmp), "%02X ", buf[i]);
        s += tmp;
      }
    log_info (std::string (label) + " HEX: " + s);
  }

  void
  stateSampleThread (void)
  {
    FILE *fp = fopen ("./stateSampleRes.txt", "w");
    if (!fp)
      {
        log_err ("Failed to open stateSampleRes.txt for writing");
        return;
      }
    while (true)
      {
        fprintf (fp, "%d,%d,%d\n", state.load (), getActiveChannelIndex (),
                 getTimeStepMs ());
        fflush (fp);
        usleep (1000); // 1ms采样一次
      }
    fclose (fp);
  }

public:
  int
  channelManagementLoop (void)
  {
    pollingChannelIndex = 0;
    state = DWELL;
    log_info ("[Init] Channel management loop start. Initial state=DWELL, "
              "pollingChannelIndex="
              << pollingChannelIndex);
    changeToChannel (0);
    log_info ("[Init] Switched to initial polling channelIndex=0");

    // 创建消息队列并写入属性
    struct mq_attr attr;
    memset (&attr, 0, sizeof (attr));
    attr.mq_flags = 0;                      // 0 表示阻塞模式
    attr.mq_maxmsg = 100;                   // 队列中最多 100 条消息
    attr.mq_msgsize = sizeof (msgPacket_t); // 单条消息大小
    attr.mq_curmsgs = 0;        // 当前消息条数（只读字段，mq_open 时忽略）
    mq_unlink (MSG_QUEUE_NAME); // 可选：启动前先清理旧队列
    log_info ("[MQ] Unlinked old message queue: " << MSG_QUEUE_NAME);
    mqd_t mq = mq_open (MSG_QUEUE_NAME, O_CREAT | O_RDONLY, 0666, &attr);
    if (mq == (mqd_t)-1)
      {
        log_err ("[MQ] Failed to open message queue");
        ;
        return -1;
      }
    log_info ("[MQ] Message queue opened: "
              << MSG_QUEUE_NAME << ", maxmsg=" << attr.mq_maxmsg
              << ", msgsize=" << attr.mq_msgsize);

    // 启动监控线程
    std::thread sampleThread (&Recver::stateSampleThread, this);
    sampleThread.detach ();

    // 启动rid_capture子进程
    start_rid_capture ();

    while (true)
      {
        if (state == DWELL)
          {
            // 非轮询频道需要切换回轮询频道
            int activeCh = getActiveChannelIndex ();
            int pollingCh = getPollingChannelIndex ();
            if (activeCh != pollingCh)
              {
                changeToChannel (pollingCh);
                log_info ("[DWELL] Active channel ("
                          << activeCh << ") != polling channel (" << pollingCh
                          << "), switched back to polling channel");
              }
            uint32_t now = getTimeStepMs ();
            uint32_t dwellElapsed = now - lastDwellTimestep;
            if (dwellElapsed < expectedDwellTime)
              { // 驻留时间内监控消息队列是否有数据包信息，如果有则记录
                mq_getattr (mq, &attr);
                if (attr.mq_curmsgs > 0)
                  {
                    for (int i = 0; i < attr.mq_curmsgs; i++)
                      { // 把队列中所有packet都取出来
                        msgPacket_t packet;
                        ssize_t recvLen = mq_receive (
                            mq, reinterpret_cast<char *> (&packet),
                            sizeof (packet), nullptr);
                        if (recvLen < 0)
                          {
                            log_err ("[DWELL] mq_receive failed, errno="
                                     << recvLen);
                          }
                        else
                          {
                            int recvedCount = recordSenderInfo (&packet);
                            if (recvedCount == 0)
                              { // 首次收到包，重置发包时间，争取接收到该发送机的下一次包
                                lastDwellTimestep = getTimeStepMs ();
                                log_info ("[DWELL] First packet from sender "
                                          << packet.ID
                                          << ", resetting dwell timer to try "
                                             "to catch "
                                             "next packet");
                              }
                          }
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
                int activeCh2 = getActiveChannelIndex ();
                if (activeCh2 != scheduledSenderInfo->channelIndex)
                  {
                    changeToChannel (scheduledSenderInfo->channelIndex);
                    int activeChAfter = getActiveChannelIndex ();
                    log_info ("[DWELL->SCHEDULE] Channel switched result: "
                              "activeCh="
                              << activeChAfter << ", targetCh="
                              << scheduledSenderInfo->channelIndex << ")");
                  }
                else
                  {
                    log_info ("[DWELL->SCHEDULE] Already on scheduled "
                              "sender's channel(id="
                              << scheduledSenderInfo->id
                              << ", ch=" << scheduledSenderInfo->channelIndex
                              << "), no switch");
                  }
                state = SCHEDULE;
                enterScheduleTimestep = getTimeStepMs ();
              }
          }
        else if (state == SCHEDULE)
          {
            uint32_t now = getTimeStepMs ();
            uint32_t scheduleElapsed = now - enterScheduleTimestep;
            if (scheduleElapsed < scheduleTimeoutTime)
              { // 调度时间内监控消息队列是否有数据包信息，如果有则记录
                mq_getattr (mq, &attr);
                if (attr.mq_curmsgs > 0)
                  {
                    msgPacket_t packet;
                    ssize_t recvLen
                        = mq_receive (mq, reinterpret_cast<char *> (&packet),
                                      sizeof (packet), nullptr);
                    if (recvLen < 0)
                      {
                        log_err ("[SCHEDULE] mq_receive failed, errno=%d"
                                 << recvLen);
                      }
                    else
                      {

                        log_info ("[SCHEDULE] Received packet during schedule "
                                  "window. curmsgs="
                                  << attr.mq_curmsgs << ", recvLen=" << recvLen
                                  << ", scheduleElapsed=" << scheduleElapsed
                                  << " ms (timeout=" << scheduleTimeoutTime
                                  << ")");
                        recordSenderInfo (&packet);
                        state = DWELL;
                      }
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