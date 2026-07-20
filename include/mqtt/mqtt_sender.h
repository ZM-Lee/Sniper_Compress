#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct mosquitto;

namespace rmcc_sniper::mqtt {

class MqttCustomByteBlockSender {
public:
    MqttCustomByteBlockSender(
        std::string host,
        int port,
        std::string topic,
        std::string client_id,
        int qos = 1);
    ~MqttCustomByteBlockSender();

    bool open();
    void close();
    bool is_open() const;
    std::string last_error() const;

    bool send_payload(const std::vector<uint8_t>& payload);

private:
    static void on_connect(mosquitto* mosq, void* userdata, int rc);
    static void on_disconnect(mosquitto* mosq, void* userdata, int rc);
    void close_locked();

    std::string host_;
    int port_;
    std::string topic_;
    std::string client_id_;
    int qos_;

    std::atomic<bool> connected_;
    std::atomic<bool> connection_result_received_;
    std::atomic<int> connection_result_;
    bool lib_initialized_;
    mosquitto* mosq_;
    std::string last_error_;
    mutable std::mutex mutex_;
    std::condition_variable connection_cv_;
};

}  // namespace rmcc_sniper::mqtt
