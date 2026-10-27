#include <Geode/Geode.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameManager.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <string>
#include <vector>

using namespace geode::prelude;
using namespace std::chrono_literals;

namespace {
    using Clock = std::chrono::steady_clock;

    // Small helper: read a setting by key.
    // bool -> bool, float -> double, int -> int64_t
    template <class T>
    T cfg(char const* key) {
        return Mod::get()->getSettingValue<T>(key);
    }

    // Shared click state (written by the input hook, read by the HUD)
    struct ClickState {
        std::deque<Clock::time_point> clicks;
        bool held = false;
    };
    ClickState g_click;

    // ---------------------------------------------------------------- FPS
    void applyFps() {
        int target = 60;
        if (cfg<bool>("fps-enabled")) {
            target = static_cast<int>(cfg<int64_t>("fps"));
        }
        target = std::max(target, 1);

        if (auto gm = GameManager::get()) {
            gm->m_customFPSTarget = target;
        }
        CCDirector::get()->setAnimationInterval(1.0 / target);
    }

    // -------------------------------------------------------- music pitch
    void applyPitch(float speed) {
        static float last = 1.f;
        float target = cfg<bool>("speed-pitch") ? speed : 1.f;
        if (target == last) return;
        last = target;

        auto engine = FMODAudioEngine::sharedEngine();
        if (!engine || !engine->m_system) return;

        FMOD::ChannelGroup* group = nullptr;
        if (engine->m_system->getMasterChannelGroup(&group) == FMOD_OK && group) {
            group->setPitch(target);
        }
    }
}

$on_mod(Loaded) {
    listenForSettingChanges<bool>("fps-enabled", [](bool) { applyFps(); });
    listenForSettingChanges<int64_t>("fps", [](int64_t) { applyFps(); });
}

// ======================================================================
// Speedhack: scale the delta time the whole game runs on
// ======================================================================
class $modify(HPScheduler, CCScheduler) {
    virtual void update(float dt) {
        float speed = 1.f;
        if (cfg<bool>("speedhack")) {
            speed = static_cast<float>(cfg<double>("speed"));
            speed = std::max(speed, 0.01f);
        }
        applyPitch(speed);
        CCScheduler::update(dt * speed);
    }
};

// ======================================================================
// Click tracking (for CPS counter and click indicator)
// ======================================================================
class $modify(HPBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        // button 1 = jump/click. Only count player 1 so dual mode isn't counted twice.
        if (button != 1 || !isPlayer1) return;

        if (down) {
            g_click.clicks.push_back(Clock::now());
            g_click.held = true;
        } else {
            g_click.held = false;
        }
    }
};

// ======================================================================
// Unlock all icons and colors
// ======================================================================
class $modify(HPGameManager, GameManager) {
    bool isIconUnlocked(int id, IconType type) {
        if (cfg<bool>("unlock-all")) return true;
        return GameManager::isIconUnlocked(id, type);
    }

    bool isColorUnlocked(int id, UnlockType type) {
        if (cfg<bool>("unlock-all")) return true;
        return GameManager::isColorUnlocked(id, type);
    }
};

// ======================================================================
// Apply FPS once the main menu loads (GD can reset it on startup)
// ======================================================================
class $modify(HPMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        applyFps();

        // Srank logo button (top-left) -> opens the mod menu settings
        auto win = CCDirector::get()->getWinSize();
        if (auto icon = CCSprite::create("srank-icon.png"_spr)) {
            icon->setScale(0.4f);
            auto btn = CCMenuItemSpriteExtra::create(icon, this, menu_selector(HPMenuLayer::onSrank));
            auto menu = CCMenu::create();
            menu->addChild(btn);
            menu->setPosition({32.f, win.height - 32.f});
            menu->setID("srank-menu"_spr);
            this->addChild(menu, 50);
        }
        return true;
    }

    void onSrank(CCObject*) {
        geode::openSettingsPopup(Mod::get(), false);
    }
};

// ======================================================================
// "Hacks" button in the pause menu -> opens this mod's settings
// ======================================================================
class $modify(HPPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        // Srank banner at the bottom of the pause screen
        if (auto banner = CCSprite::create("srank-banner.png"_spr)) {
            auto size = CCDirector::get()->getWinSize();
            banner->setScale(0.22f);
            banner->setOpacity(210);
            banner->setPosition({size.width / 2.f, 22.f});
            this->addChild(banner, 10);
        }

        auto btn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Hacks"),
            this,
            menu_selector(HPPauseLayer::onHacks)
        );

        if (auto menu = this->getChildByID("right-button-menu")) {
            menu->addChild(btn);
            menu->updateLayout();
        } else {
            auto win = CCDirector::get()->getWinSize();
            auto fallback = CCMenu::create();
            fallback->addChild(btn);
            fallback->setPosition({win.width - 60.f, 40.f});
            this->addChild(fallback);
        }
    }

    void onHacks(CCObject*) {
        geode::openSettingsPopup(Mod::get(), false);
    }
};

// ======================================================================
// Gameplay hooks: noclip, accuracy, HUD, auto coins, safe mode
// ======================================================================
class $modify(HPPlayLayer, PlayLayer) {
    struct Fields {
        int totalFrames = 0;
        int deathFrames = 0;
        bool diedThisFrame = false;

        CCLabelBMFont* cpsLabel = nullptr;
        CCLabelBMFont* accLabel = nullptr;
        CCDrawNode* indicator = nullptr;
        int lastIndicatorState = -1;
        std::string lastCps;
        std::string lastAcc;

        std::vector<GameObject*> coins;
    };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        applyFps();
        g_click = ClickState{};

        auto f = m_fields.self();
        auto win = CCDirector::get()->getWinSize();

        auto makeLabel = [&](float yFromTop) {
            auto label = CCLabelBMFont::create("", "bigFont.fnt");
            label->setScale(0.4f);
            label->setAnchorPoint({0.f, 1.f});
            label->setPosition({8.f, win.height - yFromTop});
            label->setOpacity(170);
            m_uiLayer->addChild(label, 100);
            return label;
        };
        f->cpsLabel = makeLabel(8.f);
        f->accLabel = makeLabel(26.f);

        // Faint Srank watermark (bottom-right)
        if (auto mark = CCSprite::create("srank-icon.png"_spr)) {
            mark->setScale(0.28f);
            mark->setOpacity(70);
            mark->setPosition({win.width - 24.f, 24.f});
            m_uiLayer->addChild(mark, 99);
        }

        f->indicator = CCDrawNode::create();
        f->indicator->setPosition({win.width / 2.f, 14.f});
        m_uiLayer->addChild(f->indicator, 100);

        // Cache coins once so we don't scan every object each frame
        f->coins.clear();
        for (auto obj : CCArrayExt<GameObject*>(m_objects)) {
            if (obj->m_objectType == GameObjectType::SecretCoin ||
                obj->m_objectType == GameObjectType::UserCoin) {
                f->coins.push_back(obj);
            }
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto f = m_fields.self();
        f->totalFrames = 0;
        f->deathFrames = 0;
        f->diedThisFrame = false;
    }

    // Noclip: swallow deaths (but keep the anti-cheat spike working)
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (object == m_anticheatSpike || !cfg<bool>("noclip")) {
            return PlayLayer::destroyPlayer(player, object);
        }
        m_fields->diedThisFrame = true;
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto f = m_fields.self();

        bool noclip = cfg<bool>("noclip");
        bool fast = cfg<bool>("speedhack") && cfg<double>("speed") != 1.0;
        bool coins = cfg<bool>("auto-coins");
        bool alive = m_player1 && !m_player1->m_isDead;

        // Safe mode: flag the run as a test run so cheated progress isn't saved
        if (cfg<bool>("safe-mode") && (noclip || fast || coins)) {
            m_isTestMode = true;
        }

        // ---- Noclip accuracy
        if (noclip && alive) {
            f->totalFrames++;
            if (f->diedThisFrame) f->deathFrames++;
        }
        f->diedThisFrame = false;

        float accuracy = 100.f;
        if (f->totalFrames > 0) {
            accuracy = 100.f * (1.f - static_cast<float>(f->deathFrames) / f->totalFrames);
        }

        // Optional: die when accuracy falls under the limit
        double minAcc = cfg<double>("noclip-min-accuracy");
        if (noclip && alive && minAcc > 0.0 && f->totalFrames > 120 && accuracy < minAcc) {
            PlayLayer::destroyPlayer(m_player1, m_anticheatSpike);
        }

        // ---- CPS
        auto now = Clock::now();
        while (!g_click.clicks.empty() && now - g_click.clicks.front() > 1s) {
            g_click.clicks.pop_front();
        }

        if (f->cpsLabel) {
            bool show = cfg<bool>("cps-counter");
            f->cpsLabel->setVisible(show);
            if (show) {
                auto text = fmt::format("CPS: {}", g_click.clicks.size());
                if (text != f->lastCps) {
                    f->lastCps = text;
                    f->cpsLabel->setString(text.c_str());
                }
            }
        }

        // ---- Accuracy label
        if (f->accLabel) {
            bool show = noclip && cfg<bool>("noclip-accuracy");
            f->accLabel->setVisible(show);
            if (show) {
                auto text = fmt::format("Accuracy: {:.2f}%", accuracy);
                if (text != f->lastAcc) {
                    f->lastAcc = text;
                    f->accLabel->setString(text.c_str());
                }
            }
        }

        // ---- Click indicator
        if (f->indicator) {
            bool show = cfg<bool>("click-indicator");
            f->indicator->setVisible(show);
            int state = g_click.held ? 1 : 0;
            if (show && state != f->lastIndicatorState) {
                f->lastIndicatorState = state;
                f->indicator->clear();
                ccColor4F color = state
                    ? ccc4f(0.25f, 1.f, 0.45f, 0.95f)
                    : ccc4f(1.f, 1.f, 1.f, 0.25f);
                f->indicator->drawDot({0.f, 0.f}, 8.f, color);
            }
        }

        // ---- Auto pickup coins
        // NOTE: this "touches" each coin with the player. If your Geode bindings
        // name the pickup function differently, this is the one line to change.
        if (coins && alive) {
            for (auto obj : f->coins) {
                if (obj->m_hasBeenActivated) continue;
                m_player1->collidedWithObject(dt, obj, obj->getObjectRect(), false);
            }
        }
    }
};
