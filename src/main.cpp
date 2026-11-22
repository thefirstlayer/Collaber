// FreeCollab - live collaborative level editing for Geometry Dash (Geode v5 / GD 2.2081)
//
// UI: in the editor, open the pause menu and press the "Collab" button (bottom right).
//     Type a room code, press Join. Everyone with the same code edits together.
// Sync: every second we push local changes to Supabase and pull other people's changes.

#include <Geode/Geode.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <unordered_map>
#include <random>

using namespace geode::prelude;

// Public values (the publishable key is meant to be public). Change if you move projects.
static constexpr auto SUPABASE_URL = "https://qobayzqeiltrqairxmuj.supabase.co";
static constexpr auto SUPABASE_KEY = "sb_publishable_rAImNNuAr9h0WmUyl54DDw_I8cIfPN4";
static constexpr float SYNC_INTERVAL = 1.0f;

// ---------- helpers ----------

static std::string randomHex(size_t n = 16) {
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) s += hex[rng() & 0xF];
    return s;
}

static std::string sanitize(std::string const& in) {
    std::string out;
    for (char c : in) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') out += c;
    }
    return out;
}

static std::string baseUrl() {
    return std::string(SUPABASE_URL) + "/rest/v1/collab_objects";
}

static web::WebRequest makeRequest() {
    web::WebRequest req;
    // new sb_publishable_ keys are not JWTs: send them ONLY in the apikey header
    req.header("apikey", SUPABASE_KEY);
    req.header("Content-Type", "application/json");
    return req;
}

// ---------- per-object stable id ----------

class $modify(CollabObject, GameObject) {
    struct Fields {
        std::string collabId;
    };
};

static std::string& idOf(GameObject* obj) {
    return static_cast<CollabObject*>(obj)->m_fields->collabId;
}

// ---------- shared sync state (one editor open at a time) ----------

namespace {
    struct SyncState {
        std::string clientId = randomHex(12);
        std::string room; // empty = not in a room
        long long lastSeq = 0;
        bool pushInFlight = false;
        bool pullInFlight = false;
        // uid -> last save string we know the server has (or applied locally); "" = deleted
        std::unordered_map<std::string, std::string> synced;
        // batch currently being pushed
        std::unordered_map<std::string, std::string> pending;
    };
    SyncState S;
    async::TaskHolder<web::WebResponse> pushHolder;
    async::TaskHolder<web::WebResponse> pullHolder;
}

static void resetSync() {
    pushHolder.cancel();
    pullHolder.cancel();
    S.lastSeq = 0;
    S.pushInFlight = S.pullInFlight = false;
    S.synced.clear();
    S.pending.clear();
}

static void joinRoom(std::string const& room) {
    resetSync();
    S.room = room;
    Mod::get()->setSavedValue<std::string>("room-code", room);
}

// ---------- apply remote changes ----------

static void applyRemote(LevelEditorLayer* editor, matjson::Value const& rows) {
    if (!rows.isArray()) return;

    std::unordered_map<std::string, GameObject*> byId;
    for (auto obj : CCArrayExt<GameObject*>(editor->m_objects)) {
        if (!idOf(obj).empty()) byId[idOf(obj)] = obj;
    }

    for (auto const& row : rows) {
        auto uid = row["uid"].asString().unwrapOr("");
        auto author = row["author"].asString().unwrapOr("");
        auto data = row["data"].asString().unwrapOr("");
        bool deleted = row["deleted"].asBool().unwrapOr(false);
        long long seq = row["seq"].asInt().unwrapOr(0);
        if (seq > S.lastSeq) S.lastSeq = seq;
        if (uid.empty() || author == S.clientId) continue;

        auto found = byId.find(uid);

        if (deleted) {
            if (found != byId.end()) {
                editor->removeObject(found->second, true); // API: removeObject(GameObject*, bool)
                byId.erase(found);
            }
            S.synced[uid] = "";
            continue;
        }

        if (found != byId.end()) {
            editor->removeObject(found->second, true);
            byId.erase(found);
        }

        // API: createObjectsFromString(gd::string, bool, bool) -> CCArray*
        auto created = editor->createObjectsFromString(data, true, true);
        if (created && created->count() > 0) {
            auto obj = static_cast<GameObject*>(created->objectAtIndex(0));
            idOf(obj) = uid;
            byId[uid] = obj;
            // store OUR normalized form so we don't echo it back as a "local change"
            S.synced[uid] = std::string(obj->getSaveString(editor)); // API: getSaveString(GJBaseGameLayer*)
        } else {
            S.synced[uid] = data;
        }
    }
}

// ---------- pull ----------

static void doPull() {
    if (S.pullInFlight) return;
    S.pullInFlight = true;

    auto url = fmt::format(
        "{}?room=eq.{}&seq=gt.{}&order=seq.asc&limit=500",
        baseUrl(), S.room, S.lastSeq
    );

    pullHolder.spawn(
        makeRequest().get(url),
        [](web::WebResponse res) { // runs on the main thread
            S.pullInFlight = false;
            if (!res.ok()) {
                log::warn("FreeCollab pull failed: {}", res.code());
                return;
            }
            auto editor = GameManager::get()->m_levelEditorLayer;
            if (!editor) return;
            auto json = res.json().unwrapOr(matjson::Value());
            applyRemote(editor, json);
        }
    );
}

// ---------- push (diff local objects vs last synced) ----------

static void doPush(LevelEditorLayer* editor) {
    if (S.pushInFlight) return;

    std::unordered_map<std::string, std::string> changes;
    std::unordered_map<std::string, bool> seen;

    for (auto obj : CCArrayExt<GameObject*>(editor->m_objects)) {
        auto& id = idOf(obj);
        if (id.empty()) id = randomHex(16);
        seen[id] = true;

        std::string data = obj->getSaveString(editor);
        auto f = S.synced.find(id);
        if (f == S.synced.end() || f->second != data) changes[id] = data;
    }
    for (auto const& [id, data] : S.synced) {
        if (!data.empty() && !seen.count(id)) changes[id] = "";
    }
    if (changes.empty()) return;

    std::string body = "[";
    bool first = true;
    for (auto const& [id, data] : changes) {
        if (!first) body += ",";
        first = false;
        body += fmt::format(
            R"({{"room":"{}","uid":"{}","data":"{}","deleted":{},"author":"{}"}})",
            S.room, id, data, data.empty() ? "true" : "false", S.clientId
        );
    }
    body += "]";

    S.pushInFlight = true;
    S.pending = std::move(changes);

    auto req = makeRequest();
    req.header("Prefer", "resolution=merge-duplicates,return=minimal");
    req.bodyString(body);

    pushHolder.spawn(
        req.post(baseUrl() + "?on_conflict=room,uid"),
        [](web::WebResponse res) { // runs on the main thread
            S.pushInFlight = false;
            if (res.ok()) {
                for (auto const& [id, data] : S.pending) S.synced[id] = data;
            } else {
                log::warn("FreeCollab push failed: {}", res.code());
            }
            S.pending.clear();
        }
    );
}

// ---------- editor hook: runs the sync loop ----------

class $modify(CollabEditor, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool p1) {
        if (!LevelEditorLayer::init(level, p1)) return false;

        // remember the last room, but start fresh each time the editor opens
        resetSync();
        S.room = Mod::get()->getSavedValue<std::string>("room-code", "");

        this->schedule(schedule_selector(CollabEditor::collabTick), SYNC_INTERVAL);
        if (!S.room.empty()) {
            Notification::create("FreeCollab: joined room " + S.room, NotificationIcon::Success)->show();
        }
        return true;
    }

    void collabTick(float) {
        if (S.room.empty()) return;
        // don't touch objects while the level is being play-tested
        if (this->m_playbackMode == PlaybackMode::Playing) return;
        doPush(this);
        doPull();
    }
};

// ---------- UI: popup with room code ----------

class CollabPopup : public Popup {
protected:
    TextInput* m_input = nullptr;

    bool init() {
        if (!Popup::init(300.f, 180.f)) return false;
        this->setTitle("FreeCollab");

        auto info = CCLabelBMFont::create(
            S.room.empty() ? "Not in a room" : ("In room: " + S.room).c_str(), "chatFont.fnt"
        );
        info->setScale(0.8f);
        m_mainLayer->addChildAtPosition(info, Anchor::Center, {0, 40});

        m_input = TextInput::create(200.f, "room code", "bigFont.fnt");
        m_input->setFilter("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_");
        m_input->setMaxCharCount(32);
        m_input->setString(S.room);
        m_mainLayer->addChildAtPosition(m_input, Anchor::Center, {0, 5});

        auto joinBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Join"), this, menu_selector(CollabPopup::onJoin)
        );
        m_buttonMenu->addChildAtPosition(joinBtn, Anchor::Bottom, {-50, 30});

        auto leaveBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Leave"), this, menu_selector(CollabPopup::onLeave)
        );
        m_buttonMenu->addChildAtPosition(leaveBtn, Anchor::Bottom, {50, 30});

        return true;
    }

    void onJoin(CCObject*) {
        auto room = sanitize(m_input->getString());
        if (room.empty()) {
            Notification::create("Type a room code first", NotificationIcon::Error)->show();
            return;
        }
        joinRoom(room);
        Notification::create("FreeCollab: joined room " + room, NotificationIcon::Success)->show();
        this->onClose(nullptr);
    }

    void onLeave(CCObject*) {
        resetSync();
        S.room.clear();
        Notification::create("FreeCollab: left the room", NotificationIcon::Info)->show();
        this->onClose(nullptr);
    }

public:
    static CollabPopup* create() {
        auto ret = new CollabPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------- UI: "Collab" button in the editor pause menu ----------

class $modify(CollabPause, EditorPauseLayer) {
    bool init(LevelEditorLayer* editor) {
        if (!EditorPauseLayer::init(editor)) return false;

        auto spr = ButtonSprite::create("Collab");
        spr->setScale(0.8f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(CollabPause::onCollab));

        auto menu = CCMenu::create();
        menu->addChild(btn);
        auto win = CCDirector::get()->getWinSize();
        menu->setPosition({win.width - 55.f, 30.f}); // bottom right; change if it overlaps
        this->addChild(menu);
        return true;
    }

    void onCollab(CCObject*) {
        CollabPopup::create()->show();
    }
};
