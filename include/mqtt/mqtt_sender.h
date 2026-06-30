#pragma once

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
    const std::string& last_error() const;

    bool send_payload(const std::vector<uint8_t>& payload);

private:
    std::string host_;
    int port_;
    std::string topic_;
    std::string client_id_;
    int qos_;

    bool connected_;
    bool lib_initialized_;
    mosquitto* mosq_;
    std::string last_error_;
    mutable std::mutex mutex_;
};

}  // namespace rmcc_sniper::mqtt

