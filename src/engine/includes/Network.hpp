#pragma once

#include "Component.hpp"
#include "Entity.hpp"
#include "Types.hpp"

class Network : public Component {
    Entity *entity;
    std::atomic<bool> active;
    std::string player_address;
    NetworkRole owner;

  public:
    Network(Entity *entity);

    bool GetActive();
    std::string GetPlayerAddress();
    NetworkRole GetOwner();

    void SetActive(bool active);
    void SetPlayerAddress(std::string player_address);
    void SetOwner(NetworkRole owner);

    void Update() override;
};