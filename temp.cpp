#pragma once

#include <string>
#include <deque>
#include <unordered_map>
#include <vector>
#include <limits>

class SenderInfo {
public:
    // 对应 Python deque(maxlen=100)
    static constexpr std::size_t MAX_HISTORY = 100;

    // 基本信息（对应 Python dataclass 字段）
    std::string id;               // 发送方 ID（packet_id）
    int last_sent_timestep = 0;   // 上一次收到该 sender 的时间
    int send_times = 0;           // 收到该 sender 包的次数
    int next_send_timestep = -1;  // 预测的下一次发送时间

    // 间隔相关
    std::deque<int> interval_history;               // 发包间隔历史
    bool schedule_handling = false;                 // 是否在 schedule 处理中
    int min_interval = 3600000;                     // 最小观测间隔，初始为 1h（和 Python 一致）
    int channel_index = -1;                         // 该 sender 所在的频道索引
    std::unordered_map<int, int> interval_frequency;// 间隔 -> 出现次数

public:
    SenderInfo() = default;

    SenderInfo(const std::string& id_,
               int channel_index_ = -1,
               int last_sent_timestep_ = 0,
               int send_times_ = 0,
               int next_send_timestep_ = -1)
        : id(id_),
          last_sent_timestep(last_sent_timestep_),
          send_times(send_times_),
          next_send_timestep(next_send_timestep_),
          channel_index(channel_index_) {}

    // 对应 Python: sender_info.append_interval(interval)
    void append_interval(int interval) {
        // 更新最小间隔
        if (interval < min_interval) {
            min_interval = interval;
        }

        // 如果历史长度已满，需要删除一个旧元素（保留 min_interval 的策略）
        if (interval_history.size() >= MAX_HISTORY) {
            if (!interval_history.empty()) {
                // Python 逻辑：
                // if interval_history[0] == min_interval and len >= 2:
                //     remove interval_history[1]
                // else:
                //     popleft()
                if (interval_history.front() == min_interval &&
                    interval_history.size() >= 2) {
                    int removed = interval_history[1];
                    decrement_frequency(removed);
                    interval_history.erase(interval_history.begin() + 1);
                } else {
                    int removed = interval_history.front();
                    decrement_frequency(removed);
                    interval_history.pop_front();
                }
            }
        }

        // 追加新的 interval
        interval_history.push_back(interval);
        interval_frequency[interval] += 1;
    }

    // 对应 Python 属性：last_interval
    // 没有记录时返回 -1
    int last_interval() const {
        if (interval_history.empty()) {
            return -1;
        }
        return interval_history.back();
    }

    // 对应 Python 属性：average_interval
    // 没有记录时返回 -1.0
    double average_interval() const {
        if (interval_history.empty()) {
            return -1.0;
        }
        long long sum = 0;
        for (int v : interval_history) {
            sum += v;
        }
        return static_cast<double>(sum) /
               static_cast<double>(interval_history.size());
    }

    // 对应 Python 属性：mode_interval
    // 没有记录时返回 -1
    int mode_interval() const {
        if (interval_frequency.empty()) {
            return -1;
        }
        int mode = -1;
        int best_cnt = std::numeric_limits<int>::min();
        for (const auto& kv : interval_frequency) {
            if (kv.second > best_cnt) {
                best_cnt = kv.second;
                mode = kv.first;
            }
        }
        return mode;
    }

    // 对应 Python 属性：content_interval
    std::vector<int> content_interval() const {
        return std::vector<int>(interval_history.begin(),
                                interval_history.end());
    }

private:
    // 辅助函数：减少某个间隔在 frequency 里的计数
    void decrement_frequency(int interval) {
        auto it = interval_frequency.find(interval);
        if (it != interval_frequency.end()) {
            it->second -= 1;
            if (it->second <= 0) {
                interval_frequency.erase(it);
            }
        }
    }
};
