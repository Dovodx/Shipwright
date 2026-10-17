// MotoLink multiplayer (Anchor): other players' bikes.
//
// Each PLAYER_UPDATE (sent every frame to the other players in the same scene) carries the sender's bike, when it is
// in their scene, as an optional "moto" object; clients without MotoLink ignore it. For every other player whose bike
// is in our scene we keep a ghost bike: an En_Horse bike flagged as a ghost (z_en_horse_moto.c), with no physics or
// collision, which follows the received state and plays its own positional engine with Doppler. A remote rider's
// Link (Anchor's dummy player) gets the riding pose from his ghost bike (MotoLink_GhostBikeForPlayer).
#include "MotoNet.h"
#include "MotoLink.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"
#include "soh/ShipInit.hpp"

#include <unordered_map>

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "src/overlays/actors/ovl_En_Horse/z_en_horse.h"
extern PlayState* gPlayState;
extern f32 gMotoThrottle;
}

namespace {

// clientId -> that client's ghost bike in our scene
std::unordered_map<uint32_t, Actor*> sGhosts;
// update count of the client's bike data last pushed into its ghost
std::unordered_map<uint32_t, uint32_t> sGhostSeen;

EnHorse* FindLocalBike(PlayState* play) {
    for (Actor* a = play->actorCtx.actorLists[ACTORCAT_BG].head; a != nullptr; a = a->next) {
        if (a->id == ACTOR_EN_HORSE && a->update != nullptr && ((EnHorse*)a)->type == HORSE_MOTO &&
            !((EnHorse*)a)->motoGhost) {
            return (EnHorse*)a;
        }
    }
    return nullptr;
}

bool IsLive(PlayState* play, Actor* ghost) {
    for (Actor* a = play->actorCtx.actorLists[ACTORCAT_BG].head; a != nullptr; a = a->next) {
        if (a == ghost) {
            return a->update != nullptr;
        }
    }
    return false;
}

// Writes the client's latest bike state into its ghost.
void PushState(EnHorse* ghost, const AnchorClient& c, bool fresh) {
    if (fresh) {
        ghost->motoGhostTarget = c.motoPos;
    }
    ghost->actor.world.rot.y = c.motoYaw;
    ghost->actor.speedXZ = c.motoSpeed;
    ghost->actor.velocity.y = c.motoVelY;
    ghost->motoRpm = c.motoRpm;
    ghost->motoGhostThrottle = c.motoThrottle;
    ghost->motoSteer = c.motoSteer;
    ghost->motoLean = c.motoLean;
    ghost->motoWheelie = c.motoWheelie;
    ghost->motoPitch = c.motoPitch;
    ghost->motoFallen = c.motoFallen;
    ghost->motoUnderwater = c.motoUnderwater;
    ghost->motoGhostRidden = c.motoRidden;
}

void UpdateGhosts() {
    PlayState* play = gPlayState;
    if (play == nullptr) {
        return;
    }
    Anchor* anchor = Anchor::Instance;
    bool connected = anchor != nullptr && anchor->isEnabled && anchor->isConnected;

    // Forget ghosts that went away with a scene change.
    for (auto it = sGhosts.begin(); it != sGhosts.end();) {
        if (!IsLive(play, it->second)) {
            it = sGhosts.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = sGhosts.begin(); it != sGhosts.end();) {
        bool keep = connected && anchor->clients.contains(it->first);
        if (keep) {
            const AnchorClient& c = anchor->clients[it->first];
            keep = c.online && c.isSaveLoaded && c.motoPresent && c.sceneNum == play->sceneNum && !c.self;
        }
        if (!keep) {
            Actor_Kill(it->second);
            sGhostSeen.erase(it->first);
            it = sGhosts.erase(it);
        } else {
            ++it;
        }
    }
    if (!connected) {
        return;
    }

    for (auto& [clientId, c] : anchor->clients) {
        if (c.self || !c.online || !c.isSaveLoaded || !c.motoPresent || c.sceneNum != play->sceneNum) {
            continue;
        }
        Actor* ghost;
        bool fresh = sGhostSeen[clientId] != c.motoUpdates;
        if (!sGhosts.contains(clientId)) {
            ghost = Actor_Spawn(&play->actorCtx, play, ACTOR_EN_HORSE, c.motoPos.x, c.motoPos.y, c.motoPos.z, 0,
                                c.motoYaw, 0, ENHORSE_PARAM_MOTO | ENHORSE_PARAM_MOTO_GHOST);
            if (ghost == nullptr) {
                continue;
            }
            ghost->room = -1;
            sGhosts[clientId] = ghost;
            fresh = true;
        }
        ghost = sGhosts[clientId];
        PushState((EnHorse*)ghost, c, fresh);
        sGhostSeen[clientId] = c.motoUpdates;
    }
}

void RegisterMotoNet() {
    GameInteractor::Instance->RegisterGameHook<GameInteractor::OnPlayerUpdate>(UpdateGhosts);
}

static RegisterShipInitFunc initMotoNet(RegisterMotoNet);

} // namespace

void MotoLink_AddBikeToPlayerUpdate(nlohmann::json& payload) {
    EnHorse* bike = gPlayState != nullptr ? FindLocalBike(gPlayState) : nullptr;
    if (bike == nullptr) {
        return;
    }
    nlohmann::json m;
    m["pos"] = { bike->actor.world.pos.x, bike->actor.world.pos.y, bike->actor.world.pos.z };
    m["yaw"] = bike->actor.world.rot.y;
    m["speed"] = bike->actor.speedXZ;
    m["velY"] = bike->actor.velocity.y;
    m["rpm"] = bike->motoRpm;
    m["throttle"] = bike->playerControlled ? gMotoThrottle : 0.0f;
    m["steer"] = bike->playerControlled ? bike->motoSteer : 0.0f;
    m["lean"] = bike->motoLean;
    m["wheelie"] = bike->motoWheelie;
    m["pitch"] = bike->motoPitch;
    m["fallen"] = bike->motoFallen;
    m["underwater"] = bike->motoUnderwater != 0;
    m["ridden"] = bike->playerControlled != 0;
    payload["moto"] = m;
}

void MotoLink_ReadBikeFromPlayerUpdate(AnchorClient& client, const nlohmann::json& payload) {
    if (!payload.contains("moto")) {
        client.motoPresent = false;
        return;
    }
    const nlohmann::json& m = payload["moto"];
    try {
        auto pos = m.at("pos");
        client.motoPos = { pos.at(0).get<f32>(), pos.at(1).get<f32>(), pos.at(2).get<f32>() };
        client.motoYaw = m.value("yaw", (s16)0);
        client.motoSpeed = m.value("speed", 0.0f);
        client.motoVelY = m.value("velY", 0.0f);
        client.motoRpm = m.value("rpm", 800.0f);
        client.motoThrottle = m.value("throttle", 0.0f);
        client.motoSteer = m.value("steer", 0.0f);
        client.motoLean = m.value("lean", (s16)0);
        client.motoWheelie = m.value("wheelie", (s16)0);
        client.motoPitch = m.value("pitch", (s16)0);
        client.motoFallen = m.value("fallen", (s16)0);
        client.motoUnderwater = m.value("underwater", false);
        client.motoRidden = m.value("ridden", false);
        client.motoPresent = true;
        client.motoUpdates++;
    } catch (const nlohmann::json::exception&) {
        client.motoPresent = false;
    }
}

extern "C" void MotoLink_GhostDestroyed(Actor* ghost) {
    for (auto it = sGhosts.begin(); it != sGhosts.end(); ++it) {
        if (it->second == ghost) {
            sGhostSeen.erase(it->first);
            sGhosts.erase(it);
            return;
        }
    }
}

// The ghost bike a remote player's Link (Anchor dummy player) is riding, or NULL.
extern "C" EnHorse* MotoLink_GhostBikeForPlayer(Player* player) {
    Anchor* anchor = Anchor::Instance;
    if (anchor == nullptr || player == nullptr) {
        return nullptr;
    }
    for (auto& [clientId, c] : anchor->clients) {
        if (c.player == player && sGhosts.contains(clientId)) {
            EnHorse* ghost = (EnHorse*)sGhosts[clientId];
            return ghost->motoGhostRidden ? ghost : nullptr;
        }
    }
    return nullptr;
}
