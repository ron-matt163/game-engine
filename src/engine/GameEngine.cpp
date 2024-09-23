#include "GameEngine.hpp"
#include "GameObject.hpp"
#include "SDL.h"
#include "SDL_error.h"
#include "SDL_events.h"
#include "SDL_keyboard.h"
#include "SDL_rect.h"
#include "SDL_render.h"
#include "SDL_scancode.h"
#include "SDL_video.h"
#include "Timeline.hpp"
#include "Types.hpp"
#include "Utils.hpp"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <string>
#include <thread>
#include <vector>
#include <zmq.hpp>

GameEngine::GameEngine() {
    std::signal(SIGINT, HandleSIGINT);
    app->sdl_window = nullptr;
    app->renderer = nullptr;
    app->quit = false;
    app->sigint.store(false);
    app->key_map = new KeyMap();
    app->window = Window({1920, 1080, true});

    this->game_title = "";
    this->engine_timeline = Timeline();
    this->clients_connected = 0;
    this->background_color = Color{0, 0, 0, 255};
    this->game_objects = std::vector<GameObject *>();
    this->callback = [](std::vector<GameObject *> *) {};
}

bool GameEngine::Init() {
    if (this->network_info.mode == NetworkMode::Single &&
        this->network_info.role == NetworkRole::Client) {
        return this->InitSingleClient();
    }

    if (this->network_info.mode == NetworkMode::ClientServer) {
        if (this->network_info.role == NetworkRole::Server) {
            return this->InitCSServer();
        }
        if (this->network_info.role == NetworkRole::Client) {
            return this->InitCSClient();
        }
    }

    if (this->network_info.mode == NetworkMode::PeerToPeer) {
        if (this->network_info.role == NetworkRole::Server) {
            return this->InitP2PServer();
        }
        if (this->network_info.role == NetworkRole::Peer) {
            return this->InitP2PPeer();
        }
    }

    return false;
}

bool GameEngine::InitSingleClient() {
    bool display_success = this->InitializeDisplay();
    this->ShowWelcomeScreen();
    return display_success;
}

void GameEngine::CSServerClientThread(JoinReply join_reply) {
    zmq::socket_t client_socket(this->zmq_context, zmq::socket_type::rep);
    client_socket.bind(join_reply.client_address);

    Log(LogLevel::Info, "Client thread for client [%d] started", join_reply.client_id);

    while (!app->sigint.load()) {
        try {
            zmq::message_t request;
            zmq::recv_result_t result = client_socket.recv(request, zmq::recv_flags::none);
            ObjectUpdate object_update;
            std::memcpy(&object_update, request.data(), sizeof(ObjectUpdate));

            std::string ack = "Acknowledge client [" + std::to_string(join_reply.client_id) + "]";
            zmq::message_t reply(ack.size());
            std::memcpy(reply.data(), ack.c_str(), ack.size());
            client_socket.send(reply, zmq::send_flags::none);

            GameObject *game_object = GetObjectByName(object_update.name, this->game_objects);
            game_object->SetPosition(object_update.position);
        } catch (const zmq::error_t &e) {
            Log(LogLevel::Info, "Caught error in the server client thread: %s", e.what());
            client_socket.close();
            this->server_broadcast_socket.close();
        }
    }
};

void GameEngine::CSServerBroadcastUpdates() {
    for (GameObject *game_object : this->game_objects) {
        try {
            ObjectUpdate object_update;
            std::snprintf(object_update.name, sizeof(object_update.name), "%s",
                          game_object->GetName().c_str());
            object_update.position = game_object->GetPosition();

            zmq::message_t broadcast_update(sizeof(ObjectUpdate));
            std::memcpy(broadcast_update.data(), &object_update, sizeof(ObjectUpdate));
            this->server_broadcast_socket.send(broadcast_update, zmq::send_flags::none);
        } catch (const zmq::error_t &e) {
            Log(LogLevel::Info, "Caught error while broadcasting server updates: %s", e.what());
            this->server_broadcast_socket.close();
        }
    }
}

void GameEngine::CSServerListenerThread() {
    Log(LogLevel::Info, "Server listening for incoming connections at port 5555");

    while (!app->sigint.load()) {
        try {
            zmq::message_t request;
            zmq::recv_result_t result = this->join_socket.recv(request, zmq::recv_flags::none);
            std::string message(static_cast<char *>(request.data()), request.size());

            if (message == "join") {
                int client_id = this->clients_connected += 1;

                JoinReply join_reply;
                join_reply.client_id = client_id;
                std::snprintf(join_reply.client_address, sizeof(join_reply.client_address),
                              "tcp://localhost:600%d", client_id);

                zmq::message_t reply_msg(sizeof(JoinReply));
                std::memcpy(reply_msg.data(), &join_reply, sizeof(JoinReply));
                this->join_socket.send(reply_msg, zmq::send_flags::none);

                CSServerCreateNewPlayer(client_id);

                std::thread client_thread([&, this]() { this->CSServerClientThread(join_reply); });
                client_thread.detach();
            }
        } catch (const zmq::error_t &e) {
            Log(LogLevel::Info, "Caught error in the server listener thread: %s", e.what());
            this->join_socket.close();
        }
    }
};

bool GameEngine::InitCSServer() {
    this->zmq_context = zmq::context_t(1);

    this->join_socket = zmq::socket_t(this->zmq_context, zmq::socket_type::rep);
    this->join_socket.bind("tcp://*:5555");
    std::thread listener_thread([&, this]() { this->CSServerListenerThread(); });
    listener_thread.detach();

    this->server_broadcast_socket = zmq::socket_t(this->zmq_context, zmq::socket_type::pub);
    this->server_broadcast_socket.bind("tcp://*:5556");

    return true;
}

bool GameEngine::InitCSClientConnection() {
    this->zmq_context = zmq::context_t(1);

    this->join_socket = zmq::socket_t(this->zmq_context, zmq::socket_type::req);
    this->join_socket.connect("tcp://localhost:5555");

    std::string join_message = "join";
    zmq::message_t connection_request(join_message.size());
    std::memcpy(connection_request.data(), join_message.c_str(), join_message.size());
    this->join_socket.send(connection_request, zmq::send_flags::none);

    zmq::message_t server_reply;
    zmq::recv_result_t res = this->join_socket.recv(server_reply, zmq::recv_flags::none);

    JoinReply join_reply;
    std::memcpy(&join_reply, server_reply.data(), sizeof(JoinReply));
    Log(LogLevel::Info, "The client ID assigned by the server: %s",
        std::to_string(join_reply.client_id).c_str());
    this->network_info.id = join_reply.client_id;

    this->client_update_socket = zmq::socket_t(this->zmq_context, zmq::socket_type::req);
    this->client_update_socket.connect(join_reply.client_address);

    this->server_broadcast_socket = zmq::socket_t(this->zmq_context, zmq::socket_type::sub);
    this->server_broadcast_socket.connect("tcp://localhost:5556");
    this->server_broadcast_socket.set(zmq::sockopt::subscribe, "");

    return true;
}

bool GameEngine::InitCSClient() {
    bool display_success = this->InitializeDisplay();
    bool client_connection_success = this->InitCSClientConnection();
    this->ShowWelcomeScreen();

    return display_success && client_connection_success;
}

bool GameEngine::InitP2PServer() { return false; }

bool GameEngine::InitP2PPeer() { return false; }

void GameEngine::Start() {
    if (this->network_info.mode == NetworkMode::Single &&
        this->network_info.role == NetworkRole::Client) {
        this->StartSingleClient();
    }

    if (this->network_info.mode == NetworkMode::ClientServer) {
        if (this->network_info.role == NetworkRole::Server) {
            this->StartCSServer();
        }
        if (this->network_info.role == NetworkRole::Client) {
            this->StartCSClient();
        }
    }

    if (this->network_info.mode == NetworkMode::PeerToPeer) {
        if (this->network_info.role == NetworkRole::Server) {
            this->StartP2PServer();
        }
        if (this->network_info.role == NetworkRole::Peer) {
            this->StartP2PPeer();
        }
    }
}

void GameEngine::StartSingleClient() {
    this->SetupDefaultInputs();

    std::thread input_thread = std::thread([this]() {
        while (!app->quit && !app->sigint.load()) {
            this->ReadInputsThread();
        }
    });

    this->engine_timeline.SetFrameTime(FrameTime{0, this->engine_timeline.GetTime(), 0});

    // Engine loop
    while (!app->quit && !app->sigint.load()) {
        app->quit = this->HandleEvents();
        this->GetTimeDelta();
        this->ApplyObjectPhysicsAndUpdates();
        this->TestCollision();
        this->HandleCollisions();
        this->Update();
        this->RenderScene();
    }

    if (input_thread.joinable()) {
        input_thread.join();
    }

    this->Shutdown();
}

void GameEngine::StartCSServer() {
    this->engine_timeline.SetFrameTime(FrameTime{0, this->engine_timeline.GetTime(), 0});

    while (!app->sigint.load()) {
        this->GetTimeDelta();
        this->ApplyObjectPhysicsAndUpdates();
        this->TestCollision();
        this->HandleCollisions();
        this->Update();
        this->CSServerBroadcastUpdates();
    }
}

void GameEngine::CSClientAddExistingPlayers() {
    GameObject *controllable = GetControllable(this->game_objects);
    std::string player_name = controllable->GetName() + "_" + std::to_string(this->network_info.id);

    if (this->network_info.id > 1) {
        GameObject *controllable = GetControllable(this->game_objects);

        for (int player_id = 1; player_id < this->network_info.id; player_id++) {
            GameObject *player =
                new GameObject(controllable->GetName(), controllable->GetCategory());
            std::string player_name = player->GetName() + "_" + std::to_string(player_id);
            player->SetName(player_name);
            player->SetColor(controllable->GetColor());
            player->SetSize(controllable->GetSize());
            player->SetTextureTemplate(controllable->GetTextureTemplate());
            player->SetCallback(controllable->GetCallback());
            SetPlayerTexture(player, player_id);

            this->game_objects.push_back(player);
        }
    }

    controllable->SetName(player_name);
    SetPlayerTexture(controllable, this->network_info.id);
}

GameObject *GameEngine::CSClientCreateNewPlayer(ObjectUpdate object_update) {
    GameObject *controllable = GetControllable(this->game_objects);
    GameObject *player = new GameObject(object_update.name, controllable->GetCategory());
    player->SetColor(controllable->GetColor());
    player->SetSize(controllable->GetSize());
    player->SetTextureTemplate(controllable->GetTextureTemplate());
    player->SetCallback(controllable->GetCallback());
    int player_id = GetPlayerIdFromName(object_update.name);
    SetPlayerTexture(player, player_id);

    this->game_objects.push_back(player);
    return player;
}

void GameEngine::CSServerCreateNewPlayer(int client_id) {
    GameObject *controllable = GetControllable(this->game_objects);
    if (client_id == 1) {
        controllable->SetName(Split(controllable->GetName(), '_')[0] + "_" +
                              std::to_string(client_id));
    } else {
        GameObject *player =
            new GameObject(Split(controllable->GetName(), '_')[0] + "_" + std::to_string(client_id),
                           controllable->GetCategory());
        player->SetColor(controllable->GetColor());
        player->SetSize(controllable->GetSize());
        player->SetTextureTemplate(controllable->GetTextureTemplate());
        player->SetCallback(controllable->GetCallback());
        SetPlayerTexture(player, client_id);

        this->game_objects.push_back(player);
    }
}

void GameEngine::CSClientReceiveBroadcastThread() {
    try {
        zmq::message_t message;
        zmq::recv_result_t res =
            this->server_broadcast_socket.recv(message, zmq::recv_flags::dontwait);

        if (res) {
            ObjectUpdate object_update;
            std::memcpy(&object_update, message.data(), sizeof(ObjectUpdate));
            GameObject *game_object = GetObjectByName(object_update.name, this->game_objects);
            // If the object received does not already exist in the client, create it. Occurs
            // whenever a new client joins the game
            if (game_object == nullptr) {
                game_object = this->CSClientCreateNewPlayer(object_update);
            }
            GameObject *player = GetClientPlayer(this->network_info.id, this->game_objects);

            if (game_object->GetName() != player->GetName()) {
                game_object->SetPosition(object_update.position);
            }
        }
    } catch (const zmq::error_t &e) {
        Log(LogLevel::Info, "Caught error in the client receive broadcast thread: %s", e.what());
        this->server_broadcast_socket.close();
    }
}

void GameEngine::CSClientSendUpdate() {
    try {
        GameObject *player = GetClientPlayer(this->network_info.id, this->game_objects);
        ObjectUpdate object_update;
        std::snprintf(object_update.name, sizeof(object_update.name), "%s",
                      player->GetName().c_str());
        object_update.position = player->GetPosition();

        zmq::message_t update(sizeof(ObjectUpdate));
        std::memcpy(update.data(), &object_update, sizeof(ObjectUpdate));
        this->client_update_socket.send(update, zmq::send_flags::none);

        zmq::message_t server_ack;
        zmq::recv_result_t res = this->client_update_socket.recv(server_ack, zmq::recv_flags::none);

        std::string server_ack_message(static_cast<const char *>(server_ack.data()),
                                       server_ack.size());
    } catch (const zmq::error_t &e) {
        Log(LogLevel::Info, "Caught error in the client send update thread: %s", e.what());
        this->client_update_socket.close();
    }
}

void GameEngine::StartCSClient() {
    this->CSClientAddExistingPlayers();

    this->SetupDefaultInputs();

    std::thread input_thread = std::thread([this]() {
        while (!app->quit && !app->sigint.load()) {
            this->ReadInputsThread();
        }
    });

    std::thread receive_broadcast_thread = std::thread([this]() {
        while (!app->quit && !app->sigint.load()) {
            this->CSClientReceiveBroadcastThread();
        }
    });

    this->engine_timeline.SetFrameTime(FrameTime{0, this->engine_timeline.GetTime(), 0});

    // Engine loop
    while (!app->quit && !app->sigint.load()) {
        app->quit = this->HandleEvents();
        this->GetTimeDelta();
        this->ApplyObjectPhysicsAndUpdates();
        this->TestCollision();
        this->HandleCollisions();
        this->Update();
        this->CSClientSendUpdate();
        this->RenderScene();
    }

    if (input_thread.joinable()) {
        input_thread.join();
    }

    if (receive_broadcast_thread.joinable()) {
        receive_broadcast_thread.join();
    }

    this->Shutdown();
}
void GameEngine::StartP2PServer() {}

void GameEngine::StartP2PPeer() {}

void GameEngine::SetupDefaultInputs() {
    // toggle constant and proportional scaling
    app->key_map->key_X.OnPress = [this]() {
        app->window.proportional_scaling = !app->window.proportional_scaling;
    };
    // toggle pause or unpause
    app->key_map->key_P.OnPress = [this]() {
        this->engine_timeline.TogglePause(this->engine_timeline.GetFrameTime().current);
    };
    // slow down the timeline
    app->key_map->key_comma.OnPress = [this]() {
        this->engine_timeline.ChangeTic(std::min(this->engine_timeline.GetTic() * 2.0, 2.0));
    };
    // speed up the timeline
    app->key_map->key_period.OnPress = [this]() {
        this->engine_timeline.ChangeTic(std::max(this->engine_timeline.GetTic() / 2.0, 0.5));
    };
}

bool GameEngine::InitializeDisplay() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        Log(LogLevel::Error, "SDL_Init Error: %s", SDL_GetError());
        return false;
    }

    SDL_Window *window = SDL_CreateWindow(
        this->game_title.c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        app->window.width, app->window.height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        Log(LogLevel::Error, "SDL_CreateWindow Error: %s", SDL_GetError());
        SDL_Quit();
        return false;
    } else {
        app->sdl_window = window;
    }

    SDL_Renderer *renderer =
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        SDL_DestroyWindow(window);
        Log(LogLevel::Error, "SDL_CreateRenderer Error: %s", SDL_GetError());
        SDL_Quit();
        return false;
    } else {
        app->renderer = renderer;
    }

    return true;
}

void GameEngine::SetGameTitle(std::string game_title) { this->game_title = game_title; }

void GameEngine::SetNetworkInfo(NetworkInfo network_info) { this->network_info = network_info; }

NetworkInfo GameEngine::GetNetworkInfo() { return this->network_info; }

void GameEngine::SetBackgroundColor(Color color) { this->background_color = color; }

void GameEngine::ShowWelcomeScreen() {
    // Sets the background to blue
    SDL_SetRenderDrawColor(app->renderer, this->background_color.red, this->background_color.green,
                           this->background_color.blue, 255);
    // Clears the renderer
    SDL_RenderClear(app->renderer);
    SDL_RenderPresent(app->renderer);
}

void GameEngine::AddObjects(std::vector<GameObject *> game_objects) {
    this->game_objects = game_objects;
}

void GameEngine::SetCallback(std::function<void(std::vector<GameObject *> *)> callback) {
    this->callback = callback;
}

void GameEngine::Update() { this->callback(&this->game_objects); }

void GameEngine::GetTimeDelta() {
    int64_t current = this->engine_timeline.GetTime();
    int64_t last = this->engine_timeline.GetFrameTime().last;
    int64_t delta = current - last;
    last = current;
    delta = std::clamp(delta, static_cast<int64_t>(0),
                       static_cast<int64_t>(16'000'000 / this->engine_timeline.GetTic()));

    this->engine_timeline.SetFrameTime(FrameTime{current, last, delta});
}

void GameEngine::ReadInputsThread() {
    const Uint8 *keyboard_state = SDL_GetKeyboardState(NULL);
    auto now = std::chrono::high_resolution_clock::now();

    auto debounce_key = [&](int scancode, Key &key, bool delay) {
        if (!delay) {
            key.pressed.store(keyboard_state[scancode] != 0);
            return;
        }

        if (keyboard_state[scancode] != 0) {
            auto press_duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - key.last_pressed_time);
            if (press_duration.count() > 50 && !key.pressed.load()) {
                key.pressed.store(true);
                key.OnPress();
            } else {
                key.pressed.store(false);
                key.last_pressed_time = now;
            }
        } else {
            key.pressed.store(false);
        }
    };

    debounce_key(SDL_SCANCODE_X, app->key_map->key_X, true);
    debounce_key(SDL_SCANCODE_P, app->key_map->key_P, true);
    debounce_key(SDL_SCANCODE_COMMA, app->key_map->key_comma, true);
    debounce_key(SDL_SCANCODE_PERIOD, app->key_map->key_period, true);

    debounce_key(SDL_SCANCODE_W, app->key_map->key_W, false);
    debounce_key(SDL_SCANCODE_A, app->key_map->key_A, false);
    debounce_key(SDL_SCANCODE_S, app->key_map->key_S, false);
    debounce_key(SDL_SCANCODE_D, app->key_map->key_D, false);

    debounce_key(SDL_SCANCODE_UP, app->key_map->key_up, false);
    debounce_key(SDL_SCANCODE_LEFT, app->key_map->key_left, false);
    debounce_key(SDL_SCANCODE_DOWN, app->key_map->key_down, false);
    debounce_key(SDL_SCANCODE_RIGHT, app->key_map->key_right, false);

    debounce_key(SDL_SCANCODE_SPACE, app->key_map->key_space, false);
}

void GameEngine::ApplyObjectPhysicsAndUpdates() {
    std::vector<GameObject *> game_objects =
        GetObjectsByRole(this->network_info, this->game_objects);

    for (GameObject *game_object : game_objects) {
        game_object->Move(this->engine_timeline.GetFrameTime().delta);
        game_object->Update();
    }
}

void GameEngine::TestCollision() {
    for (int i = 0; i < this->game_objects.size() - 1; i++) {
        for (int j = i + 1; j < this->game_objects.size(); j++) {
            SDL_Rect object_1 = {
                static_cast<int>(std::round(this->game_objects[i]->GetPosition().x)),
                static_cast<int>(std::round(this->game_objects[i]->GetPosition().y)),
                this->game_objects[i]->GetSize().width, this->game_objects[i]->GetSize().height};
            SDL_Rect object_2 = {
                static_cast<int>(std::round(this->game_objects[j]->GetPosition().x)),
                static_cast<int>(std::round(this->game_objects[j]->GetPosition().y)),
                this->game_objects[j]->GetSize().width, this->game_objects[j]->GetSize().height};

            if (SDL_HasIntersection(&object_1, &object_2)) {
                this->game_objects[i]->AddCollider(this->game_objects[j]);
                this->game_objects[j]->AddCollider(this->game_objects[i]);
            } else {
                this->game_objects[i]->RemoveCollider(this->game_objects[j]);
                this->game_objects[j]->RemoveCollider(this->game_objects[i]);
            }
        }
    }
}

void GameEngine::HandleCollisions() {
    std::vector<std::thread> threads;

    auto handle_collision = [&](GameObject *game_object) {
        if (game_object->GetColliders().size() > 0) {
            for (GameObject *collider : game_object->GetColliders()) {
                int obj_x = static_cast<int>(std::round(game_object->GetPosition().x));
                int obj_y = static_cast<int>(std::round(game_object->GetPosition().y));

                int col_x = static_cast<int>(std::round(collider->GetPosition().x));
                int col_y = static_cast<int>(std::round(collider->GetPosition().y));

                int obj_width = game_object->GetSize().width;
                int obj_height = game_object->GetSize().height;
                int col_width = collider->GetSize().width;
                int col_height = collider->GetSize().height;

                int left_overlap = (obj_x + obj_width) - col_x;
                int right_overlap = (col_x + col_width) - obj_x;
                int top_overlap = (obj_y + obj_height) - col_y;
                int bottom_overlap = (col_y + col_height) - obj_y;

                int min_overlap = std::min(std::min(left_overlap, right_overlap),
                                           std::min(top_overlap, bottom_overlap));

                int pos_x = 0, pos_y = 0;
                if (min_overlap == left_overlap) {
                    pos_x = col_x - obj_width;
                    pos_y = obj_y;
                } else if (min_overlap == right_overlap) {
                    pos_x = col_x + col_width;
                    pos_y = obj_y;
                } else if (min_overlap == top_overlap) {
                    pos_x = obj_x;
                    pos_y = col_y - obj_height;
                } else if (min_overlap == bottom_overlap) {
                    pos_x = obj_x;
                    pos_y = col_y + col_height;
                }

                game_object->SetPosition(Position{float(pos_x), float(pos_y)});

                float vel_x = game_object->GetVelocity().x;
                float vel_y = game_object->GetVelocity().y;
                if (min_overlap == left_overlap || min_overlap == right_overlap) {
                    vel_x *= -game_object->GetRestitution();
                }
                if (min_overlap == top_overlap || min_overlap == bottom_overlap) {
                    vel_y *= -game_object->GetRestitution();
                }
                game_object->SetVelocity(Velocity{vel_x, vel_y});
            }
        }
    };

    // Iterate over each game object
    for (GameObject *game_object : GetObjectsByRole(this->network_info, this->game_objects)) {
        if (game_object->GetAffectedByCollision()) {
            // Spawn a new thread for each game object
            threads.emplace_back(handle_collision, game_object);
        }
    }

    // Join all threads
    for (auto &thread : threads) {
        thread.join();
    }
}

bool GameEngine::HandleEvents() {
    SDL_Event event;
    bool quit = false;
    while (SDL_PollEvent(&event) != 0) {
        if (event.type == SDL_QUIT) {
            quit = true;
        }
    }
    return quit;
}

void GameEngine::RenderScene() {
    this->HandleScaling();

    this->RenderBackground();
    for (GameObject *game_object : this->game_objects) {
        game_object->Render();
    }
    SDL_RenderPresent(app->renderer);
}

void GameEngine::RenderBackground() {
    // Add conditions to change the background later
    // Add options to render an image as a background later
    SDL_SetRenderDrawColor(app->renderer, this->background_color.red, this->background_color.green,
                           this->background_color.blue, 255);
    SDL_RenderClear(app->renderer);
}

void GameEngine::HandleScaling() {
    int set_logical_size_err;

    if (app->window.proportional_scaling) {
        set_logical_size_err =
            SDL_RenderSetLogicalSize(app->renderer, app->window.width, app->window.height);
    } else {
        set_logical_size_err = SDL_RenderSetLogicalSize(app->renderer, 0, 0);
    }

    if (set_logical_size_err) {
        Log(LogLevel::Error, "Set Viewport failed: %s", SDL_GetError());
    }
}

void GameEngine::Shutdown() {
    this->join_socket.close();
    this->server_broadcast_socket.close();
    this->client_update_socket.close();
    SDL_DestroyRenderer(app->renderer);
    SDL_DestroyWindow(app->sdl_window);
    SDL_Quit();
    delete app->key_map;
    delete app;
}