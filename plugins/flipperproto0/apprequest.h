#pragma once

#include "mainrequest.h"

// Loader/App RPC. These give BLE what run_cli gives USB for opening an app:
// App_Start launches a built-in or installed app by name (the RPC form of
// `loader open <App>`), App_Exit closes whatever is running (`loader close`).
// The Flipper does not carry its text CLI over Bluetooth, but it does carry
// these over the same protobuf RPC every other wireless operation uses.
class AppStartRequest : public MainRequest
{
public:
    AppStartRequest(uint32_t id, const QByteArray &name, const QByteArray &args);
private:
    QByteArray m_name;
    QByteArray m_args;
};

class AppExitRequest : public MainRequest
{
public:
    AppExitRequest(uint32_t id);
};
