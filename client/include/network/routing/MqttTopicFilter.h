#pragma once

#include <QString>

class MqttTopicFilter final {
public:
    static bool matches(const QString& filter, const QString& topic);
    static int integerWildcardValue(const QString& filter, const QString& topic, int minimum, int maximum);
};
