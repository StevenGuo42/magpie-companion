#pragma once

#include "app_state.h"

struct NetMessage {
  char text[49];
};

bool netBegin();
bool netConnected();
void netRequestRefresh();
void netReconnect();
// Push-button WPS; the network it joins is saved and used on every boot.
bool netStartWps();
bool netWpsActive();
bool netTakeState(AppState& state);
bool netTakeMessage(NetMessage& message);
