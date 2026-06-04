#include "MqttClient.hpp"
#include <mosquitto.h>
#include <iostream>
#include <stdexcept>

// One-time library init/cleanup tied to static storage duration.
namespace {
struct MosqLib {
    MosqLib()  { mosquitto_lib_init(); }
    ~MosqLib() { mosquitto_lib_cleanup(); }
} lib_guard;
} // namespace

void MqttClient::onDisconnect(struct mosquitto *, void *userdata, int)
{
    static_cast<MqttClient *>(userdata)->connected_ = false;
}

MqttClient::MqttClient(const MqttConfig &cfg) : cfg_(cfg)
{
    mosq_ = mosquitto_new(cfg_.client_id.c_str(), true, this);
    if (!mosq_)
        throw std::runtime_error("[MQTT] mosquitto_new failed (out of memory)");
    mosquitto_disconnect_callback_set(mosq_, onDisconnect);
}

MqttClient::~MqttClient()
{
    disconnect();
    mosquitto_destroy(mosq_);
}

bool MqttClient::connect()
{
    int rc = mosquitto_connect(mosq_, cfg_.broker.c_str(), cfg_.port, cfg_.keepalive);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "[MQTT] Connect failed: " << mosquitto_strerror(rc) << "\n";
        return false;
    }
    mosquitto_loop_start(mosq_);   // background network thread
    connected_ = true;
    std::cout << "[MQTT] Connected to " << cfg_.broker << ":" << cfg_.port << "\n";
    return true;
}

void MqttClient::disconnect()
{
    if (connected_) {
        mosquitto_disconnect(mosq_);
        mosquitto_loop_stop(mosq_, false);
        connected_ = false;
    }
}

bool MqttClient::isConnected() const
{
    return connected_;
}

bool MqttClient::publish(const std::string &topic, const std::string &payload)
{
    if (!isConnected() && !reconnect())
        return false;

    int rc = mosquitto_publish(mosq_, nullptr,
                               topic.c_str(),
                               static_cast<int>(payload.size()),
                               payload.c_str(),
                               cfg_.qos, cfg_.retain);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "[MQTT] Publish failed on " << topic << ": "
                  << mosquitto_strerror(rc) << "\n";
        return false;
    }
    return true;
}

bool MqttClient::reconnect()
{
    int rc = mosquitto_reconnect(mosq_);
    if (rc == MOSQ_ERR_SUCCESS) {
        connected_ = true;
        return true;
    }
    std::cerr << "[MQTT] Reconnect failed: " << mosquitto_strerror(rc) << "\n";
    return false;
}
