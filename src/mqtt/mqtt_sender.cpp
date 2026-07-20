#include "mqtt/mqtt_sender.h"

#include "protobuf/rmcc.pb.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

#include <mosquitto.h>

namespace rmcc_sniper::mqtt {

MqttCustomByteBlockSender::MqttCustomByteBlockSender(
    std::string host,
    int port,
    std::string topic,
    std::string client_id,
    int qos)
    : host_(std::move(host)),
      port_(port),
      topic_(std::move(topic)),
      client_id_(std::move(client_id)),
      qos_(qos),
      connected_(false),
      connection_result_received_(false),
      connection_result_(MOSQ_ERR_UNKNOWN),
      lib_initialized_(false),
      mosq_(nullptr),
      last_error_() {}

MqttCustomByteBlockSender::~MqttCustomByteBlockSender() {
    close();
}

bool MqttCustomByteBlockSender::open() {
    std::unique_lock<std::mutex> lock(mutex_);
    close_locked();
    last_error_.clear();
    connection_result_received_ = false;
    connection_result_ = MOSQ_ERR_UNKNOWN;

    const int lib_rc = mosquitto_lib_init();
    if (lib_rc != MOSQ_ERR_SUCCESS) {
        last_error_ = std::string("mosquitto_lib_init failed: ") + mosquitto_strerror(lib_rc);
        return false;
    }
    lib_initialized_ = true;

    mosq_ = mosquitto_new(client_id_.empty() ? nullptr : client_id_.c_str(), true, this);
    if (mosq_ == nullptr) {
        last_error_ = "mosquitto_new failed";
        mosquitto_lib_cleanup();
        lib_initialized_ = false;
        return false;
    }

    mosquitto_int_option(mosq_, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V311);
    mosquitto_connect_callback_set(mosq_, &MqttCustomByteBlockSender::on_connect);
    mosquitto_disconnect_callback_set(mosq_, &MqttCustomByteBlockSender::on_disconnect);
    mosquitto_reconnect_delay_set(mosq_, 1, 10, true);

    const int conn_rc = mosquitto_connect(mosq_, host_.c_str(), port_, 60);
    if (conn_rc != MOSQ_ERR_SUCCESS) {
        if (conn_rc == MOSQ_ERR_ERRNO) {
            last_error_ = std::string("mosquitto_connect errno: ") + std::strerror(errno);
        } else {
            last_error_ = std::string("mosquitto_connect failed: ") + mosquitto_strerror(conn_rc);
        }
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
        mosquitto_lib_cleanup();
        lib_initialized_ = false;
        return false;
    }

    const int loop_rc = mosquitto_loop_start(mosq_);
    if (loop_rc != MOSQ_ERR_SUCCESS) {
        last_error_ = std::string("mosquitto_loop_start failed: ") + mosquitto_strerror(loop_rc);
        mosquitto_disconnect(mosq_);
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
        mosquitto_lib_cleanup();
        lib_initialized_ = false;
        return false;
    }

    const bool got_connack = connection_cv_.wait_for(
        lock,
        std::chrono::seconds(5),
        [this]() { return connection_result_received_.load(); });
    if (!got_connack) {
        last_error_ = "timed out waiting for MQTT CONNACK";
        close_locked();
        return false;
    }
    if (connection_result_ != 0) {
        last_error_ = std::string("MQTT connection refused: ") +
            mosquitto_connack_string(connection_result_);
        close_locked();
        return false;
    }
    return true;
}

void MqttCustomByteBlockSender::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
}

void MqttCustomByteBlockSender::close_locked() {
    if (mosq_ != nullptr) {
        mosquitto_loop_stop(mosq_, true);
        mosquitto_disconnect(mosq_);
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
    }
    connected_ = false;

    if (lib_initialized_) {
        mosquitto_lib_cleanup();
        lib_initialized_ = false;
    }
}

bool MqttCustomByteBlockSender::is_open() const {
    return connected_.load();
}

bool MqttCustomByteBlockSender::send_payload(const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_ || mosq_ == nullptr) {
        last_error_ = "MQTT connection unavailable; reconnecting in background";
        return false;
    }

    rmcc::CustomByteBlock msg;
    if (payload.empty()) {
        msg.set_data(std::string{});
    } else {
        msg.set_data(std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
    }

    std::string serialized;
    if (!msg.SerializeToString(&serialized)) {
        last_error_ = "CustomByteBlock SerializeToString failed";
        return false;
    }

    const int rc = mosquitto_publish(
        mosq_,
        nullptr,
        topic_.c_str(),
        static_cast<int>(serialized.size()),
        serialized.empty() ? nullptr : serialized.data(),
        qos_,
        false);
    if (rc != MOSQ_ERR_SUCCESS) {
        last_error_ = std::string("mosquitto_publish failed: ") + mosquitto_strerror(rc);
    }
    return rc == MOSQ_ERR_SUCCESS;
}

std::string MqttCustomByteBlockSender::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

void MqttCustomByteBlockSender::on_connect(mosquitto*, void* userdata, int rc) {
    auto* self = static_cast<MqttCustomByteBlockSender*>(userdata);
    self->connection_result_ = rc;
    self->connection_result_received_ = true;
    self->connected_ = (rc == 0);
    self->connection_cv_.notify_all();
}

void MqttCustomByteBlockSender::on_disconnect(mosquitto*, void* userdata, int) {
    auto* self = static_cast<MqttCustomByteBlockSender*>(userdata);
    self->connected_ = false;
}

}  // namespace rmcc_sniper::mqtt
