#include "log.hpp"
#include <deque>
#include <limits>
#include <string>
#include <unordered_map>

class SenderInfo
{
public:
  static constexpr std::size_t MAX_HISTORY_SIZE = 100;

public:
  std::string id = "";             // 接收机ID
  int lastSentTimestep = -1;       // 上次收到数据包的时间戳
  int lastInterval = -1;           // 最近一次收到数据包的间隔
  int recvedTimes = 0;             // 接收到该接收机的数据包的次数
  int nextSendTimestep = -1;       // 预计下次发送数据包的时间戳
  std::deque<int> intervalHistory; // 存放每次收到数据包间隔历史的队列
  bool scheduleHandling = false;   // 该调度包已被其他接收机处理的标志位
  int minInterval = 3600000;       // 最小接收间隔，初始化为1小时
  int channelIndex = -1;           // 接收机所在频道
  std::unordered_map<int, int> intervalFrequency;

public:
  SenderInfo () = default;

  SenderInfo (const std::string &id_, int channelIndex_ = -1,
              int recvedTimes_ = 0, int lastSentTimestep_ = 0,
              int nextSendTimestep_ = -1)
      : id (id_), lastSentTimestep (lastSentTimestep_),
        recvedTimes (recvedTimes_), nextSendTimestep (nextSendTimestep_),
        channelIndex (channelIndex_)
  {
    log_info ("Sender " + id + " Created.");
  }

  void
  appendInterval (int interval_)
  {
    // 维护间隔历史、最小间隔和各间隔频率字典
    if (interval_ < minInterval)
      minInterval = interval_;
    // 保证历史长度为MAX_HISTORY_SIZE
    if (intervalHistory.size () >= MAX_HISTORY_SIZE)
      {
        if (!intervalHistory.empty ())
          {
            int removed = intervalHistory.front ();
            decrement_frequency (removed);
            intervalHistory.pop_front ();
          }
      }
    intervalHistory.push_back (interval_);
    intervalFrequency[interval_] += 1;
  }

  int
  getLastInterval (void) const
  {
    if (intervalHistory.empty ())
      return -1;
    return intervalHistory.back ();
  }

  float
  getAverageInterval (void) const
  {
    if (intervalHistory.empty ())
      return -1;

    long long sum = 0;
    for (int val : intervalHistory)
      sum += val;

    return static_cast<float> (sum)
           / static_cast<float> (intervalHistory.size ());
  }

  int
  getModeInterval (void) const
  {
    if (intervalHistory.empty ())
      return -1;

    int mode = -1;
    int maxKeyVal = std::numeric_limits<int>::min ();
    for (const auto &kv : intervalFrequency)
      {
        if (kv.second > maxKeyVal)
          {
            maxKeyVal = kv.second;
            mode = kv.first;
          }
      }
    return mode;
  }

private:
  // 辅助函数：减少某个间隔在 frequency 里的计数
  void
  decrement_frequency (int interval)
  {
    auto it = intervalFrequency.find (interval);
    if (it != intervalFrequency.end ())
      {
        it->second -= 1;
        if (it->second <= 0)
          {
            intervalFrequency.erase (it);
          }
      }
  }
};

typedef std::unordered_map<std::string, SenderInfo> SenderInfoMap;