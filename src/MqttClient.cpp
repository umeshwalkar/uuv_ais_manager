#include "MqttClient.hpp"
#include "Logger.hpp"
#include <mosquitto.h>
#include <stdexcept>

#define MOD "MqttClient"

// One-time library init/cleanup tied to static storage duration.
namespace {
struct MosqLib {
    MosqLib()  { mosquitto_lib_init(); }
    ~MosqLib() { mosquitto_lib_cleanup(); }
} lib_guard;
} // namespace

void MqttClient::onDisconnect(struct mosquitto*, void* userdata, int rc)
{
    auto* self = static_cast<MqttClient*>(userdata);
    self->connected_ = false;
    if (rc != 0)
        LOG_WRN(MOD, "Unexpected disconnect from %s:%d (rc=%d)",
                self->cfg_.broker.c_str(), self->cfg_.port, rc);
    else
        LOG_INF(MOD, "Disconnected from %s:%d", self->cfg_.broker.c_str(), self->cfg_.port);
}

MqttClient::MqttClient(const MqttConfig& cfg) : cfg_(cfg)
{
    mosq_ = mosquitto_new(cfg_.client_id.c_str(), true, this);
    if (!mosq_) {
        LOG_ERR(MOD, "mosquitto_new failed — out of memory");
        throw std::runtime_error("mosquitto_new failed");
    }
    mosquitto_disconnect_callback_set(mosq_, onDisconnect);
    LOG_DBG(MOD, "Created client id='%s'", cfg_.client_id.c_str());
}

MqttClient::~MqttClient()
{
    disconnect();
    mosquitto_destroy(mosq_);
}

bool MqttClient::connect()
{
    LOG_INF(MOD, "Connecting to %s:%d  keepalive=%ds  client='%s'",
            cfg_.broker.c_str(), cfg_.port, cfg_.keepalive, cfg_.client_id.c_str());
    int rc = mosquitto_connect(mosq_, cfg_.broker.c_str(), cfg_.port, cfg_.keepalive);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERR(MOD, "Connect FAILED to %s:%d — %s",
                cfg_.broker.c_str(), cfg_.port, mosquitto_strerror(rc));
        return false;
    }
    mosquitto_loop_start(mosq_);
    connected_ = true;
    LOG_INF(MOD, "Connected to %s:%d", cfg_.broker.c_str(), cfg_.port);
    return true;
}

void MqttClient::disconnect()
{
    if (connected_) {
        mosquitto_disconnect(mosq_);
        mosquitto_loop_stop(mosq_, false);
        connected_ = false;
        LOG_INF(MOD, "Disconnected (client requested)");
    }
}

bool MqttClient::isConnected() const { return connected_; }

bool MqttClient::publish(const std::string& topic, const std::string& payload)
{
    if (!isConnected() && !reconnect())
        return false;

    int rc = mosquitto_publish(mosq_, nullptr,
                               topic.c_str(),
                               static_cast<int>(payload.size()),
                               payload.c_str(),
                               cfg_.qos, cfg_.retain);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERR(MOD, "Publish FAILED on '%s': %s", topic.c_str(), mosquitto_strerror(rc));
        return false;
    }

    LOG_DBG(MOD, "TX [%s]  %zu bytes  qos=%d", topic.c_str(), payload.size(), cfg_.qos);
    return true;
}

bool MqttClient::reconnect()
{
    LOG_WRN(MOD, "Not connected — attempting reconnect to %s:%d",
            cfg_.broker.c_str(), cfg_.port);
    int rc = mosquitto_reconnect(mosq_);
    if (rc == MOSQ_ERR_SUCCESS) {
        connected_ = true;
        LOG_INF(MOD, "Reconnected to %s:%d", cfg_.broker.c_str(), cfg_.port);
        return true;
    }
    LOG_ERR(MOD, "Reconnect FAILED: %s", mosquitto_strerror(rc));
    return false;
}
