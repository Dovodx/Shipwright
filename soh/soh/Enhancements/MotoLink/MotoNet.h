#pragma once
// MotoLink multiplayer: other Anchor players' bikes, drawn as ghost bikes (see MotoNet.cpp).
#ifdef __cplusplus
#include <nlohmann/json.hpp>
#include "soh/Network/Anchor/Anchor.h"

// PLAYER_UPDATE: adds the local player's bike (when it's in their scene) as payload["moto"]
void MotoLink_AddBikeToPlayerUpdate(nlohmann::json& payload);
// PLAYER_UPDATE received: stores the sender's bike state in their AnchorClient
void MotoLink_ReadBikeFromPlayerUpdate(AnchorClient& client, const nlohmann::json& payload);
#endif
