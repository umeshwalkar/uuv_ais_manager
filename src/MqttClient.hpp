#pragma once
#include <string>
#include <functional>
#include "Config.hpp"

struct mosquitto;
struct mosquitto_message;

class MqttClient
{
public:
    using MessageCallback = std::function<void(const std::string& topic, const std::string& payload)>;

    explicit MqttClient(const MqttConfig &cfg);
    ~MqttClient();

    bool connect();
    void disconnect();
    bool isConnected() const;

    bool publish(const std::string &topic, const std::string &payload);
    void subscribe(const std::string& topic, int qos = 1);
    void setMessageCallback(MessageCallback cb);

private:
    MqttConfig       cfg_;
    struct mosquitto *mosq_      = nullptr;
    bool             connected_  = false;
    MessageCallback  msg_cb_;

    bool reconnect();

    static void onDisconnect(struct mosquitto *, void *userdata, int rc);
    static void onMessage(struct mosquitto *, void *userdata, const struct mosquitto_message *msg);
};
