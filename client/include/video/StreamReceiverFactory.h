#pragma once

#include <QtGui/qwindowdefs.h>

#include <memory>

class StreamReceiver;

class StreamReceiverFactory {
public:
    virtual ~StreamReceiverFactory() = default;

    virtual std::shared_ptr<StreamReceiver> create(WId outputWindowHandle) const = 0;
};
