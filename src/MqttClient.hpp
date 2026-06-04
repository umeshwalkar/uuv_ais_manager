#pragma once
#include <string>
#include "Config.hpp"

struct mosquitto;

class MqttClient
{
public:
    explicit MqttClient(const MqttConfig &cfg);
    ~MqttClient();

    bool connect();
    void disconnect();
    bool isConnected() const;

    bool publish(const std::string &topic, const std::string &payload);

private:
    MqttConfig       cfg_;
    struct mosquitto *mosq_      = nullptr;
    bool             connected_  = false;

    bool reconnect();

    static void onDisconnect(struct mosquitto *, void *userdata, int rc);
};
