// Auto Frame Window — measures every input's frame window automatically.
//
// How it works
//   1. RECORD   Play the level in normal mode. Every handleButton call is logged
//               as (frame, down, button, player).
//   2. LAY      Pause -> "Frame Windows". The mod switches to practice mode,
//               replays your inputs from the start and drops a practice
//               checkpoint every N frames. If the replay dies early, the run is
//               not deterministic (random triggers etc.) and analysis aborts.
//   3. TRIALS   For each input, last to first: respawn at the nearest checkpoint
//               before it, replay everything with that ONE input shifted by
//               -1, -2, ... frames until death (early edge), then +1, +2, ...
//               (late edge). Window = early + late + 1.
//   4. DONE     Summary popup + CSV in the mod's save dir.
//
// Frame = int(levelTime * TPS) + 1, same convention as xdBot, so timings are
// consistent between the recording hook and the injection hook.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/CCScheduler.hpp>
#include <Geode/utils/file.hpp>

#include <algorithm>
#include <map>
#include <vector>

using namespace geode::prelude;

// ---------------------------------------------------------------- settings
static int   cfgTPS()        { return (int)Mod::get()->getSettingValue<int64_t>("tps"); }
static int   cfgCpInterval() { return (int)Mod::get()->getSettingValue<int64_t>("checkpoint-interval"); }
static int   cfgLead()       { return (int)Mod::get()->getSettingValue<int64_t>("lead-frames"); }
static int   cfgLookahead()  { return (int)Mod::get()->getSettingValue<int64_t>("lookahead-frames"); }
static int   cfgMaxShift()   { return (int)Mod::get()->getSettingValue<int64_t>("max-shift"); }
static float cfgSpeed()      { return (float)Mod::get()->getSettingValue<double>("analysis-speed"); }
static bool  cfgRemoveArg()  { return Mod::get()->getSettingValue<bool>("remove-checkpoint-arg"); }
static int   cfgInjectOff()  { return (int)Mod::get()->getSettingValue<int64_t>("inject-offset"); }

// ---------------------------------------------------------------- state
struct InputEv {
    int  frame;
    bool down;
    int  button;
    bool p1;
};

struct WindowResult {
    int  early = 0, late = 0;
    bool earlyCapped = false, lateCapped = false;
    int  window() const { return early + late + 1; }
};

enum class Phase { Idle, Recording, Laying, Trials, Done };

struct AFW {
    Phase phase = Phase::Idle;

    // recording
    std::vector<InputEv> buffer;        // run currently being recorded
    std::vector<InputEv> lastRun;       // most recent run with >= 1 input
    std::vector<InputEv> lastCompleted; // most recent completed run
    int  bufferDeathFrame = -1;
    int  lastRunDeathFrame = -1;      // -1 = completed
    int  lastLevelID = -1;

    // analysis
    std::vector<InputEv>  macro;        // run under analysis
    std::vector<InputEv>  trialEvents;  // macro with one event shifted
    std::vector<WindowResult> results;
    std::vector<int>    cpFrames;     // frame of each practice checkpoint, index == position in m_checkpointArray
    std::vector<CheckpointObject*> cpObjs;
    int    macroEnd = 0;              // frame after which nothing is measured
    size_t cursor = 0;
    int    lastInjectFrame = -1;
    int    lastCpFrame = 0;
    bool   injecting = false;
    bool   pendingStart = false;
    bool   layingEndedByDeath = false;

    // trial
    int  idx = -1;
    int  shift = 0;
    bool earlySide = true;
    int  trialEnd = 0;
    int  pending = 0;                 // 0 none, 1 died, 2 survived
    int  trialsRun = 0;

    static AFW& get() { static AFW s; return s; }

    void resetAnalysis() {
        macro.clear(); trialEvents.clear(); results.clear();
        cpFrames.clear(); cpObjs.clear();
        cursor = 0; lastInjectFrame = -1; lastCpFrame = 0;
        injecting = false; pendingStart = false; layingEndedByDeath = false;
        idx = -1; shift = 0; earlySide = true; trialEnd = 0; pending = 0; trialsRun = 0;
    }
    bool analysing() const { return phase == Phase::Laying || phase == Phase::Trials; }
};

static int curFrame(GJBaseGameLayer* l) {
    return (int)(l->m_gameState.m_levelTime * cfgTPS()) + 1;
}

// ---------------------------------------------------------------- helpers
static void abortAnalysis(std::string const& why) {
    auto& s = AFW::get();
    log::error("analysis aborted: {}", why);
    s.phase = Phase::Idle;
    s.injecting = false;
    FLAlertLayer::create("Frame Windows", "Analysis aborted:\n" + why, "OK")->show();
}

// Same (button, player) neighbours bound how far an event can move.
static int earlyBound(AFW& s, int i) {
    auto const& e = s.macro[i];
    int b = -cfgMaxShift();
    for (int j = i - 1; j >= 0; --j) {
        if (s.macro[j].button == e.button && s.macro[j].p1 == e.p1) {
            b = std::max(b, s.macro[j].frame - e.frame + 1);
            break;
        }
    }
    b = std::max(b, 1 - e.frame);
    return std::min(b, 0);
}

static int lateBound(AFW& s, int i) {
    auto const& e = s.macro[i];
    int b = cfgMaxShift();
    for (size_t j = i + 1; j < s.macro.size(); ++j) {
        if (s.macro[j].button == e.button && s.macro[j].p1 == e.p1) {
            b = std::min(b, s.macro[j].frame - e.frame - 1);
            break;
        }
    }
    b = std::min(b, s.macroEnd - 1 - e.frame);
    return std::max(b, 0);
}

static void nextInput(AFW& s) {
    s.idx--;
    s.earlySide = true;
    s.shift = -1;
    if (s.idx < 0) s.phase = Phase::Done;
}

// Skip sides that have no room to move (e.g. the same key was pressed on the previous frame).
static void normalizeTrial(AFW& s) {
    while (s.phase == Phase::Trials) {
        if (s.earlySide && s.shift < earlyBound(s, s.idx))      { s.earlySide = false; s.shift = 1; continue; }
        if (!s.earlySide && s.shift > lateBound(s, s.idx))      { nextInput(s); continue; }
        break;
    }
}

// Consume the result of the trial that just ended and pick the next (idx, shift).
// "capped" = we stopped because we hit max-shift / a neighbouring input, not a death.
static void applyPendingResult(AFW& s) {
    auto& r = s.results[s.idx];
    bool survived = (s.pending == 2);
    s.pending = 0;
    s.trialsRun++;
    if (s.earlySide) {
        if (survived) r.early = -s.shift;
        if (!survived || s.shift - 1 < earlyBound(s, s.idx)) {
            if (survived) r.earlyCapped = true;
            s.earlySide = false; s.shift = 1;
        } else s.shift--;
    } else {
        if (survived) r.late = s.shift;
        if (!survived || s.shift + 1 > lateBound(s, s.idx)) {
            if (survived) r.lateCapped = true;
            nextInput(s);
        } else s.shift++;
    }
    normalizeTrial(s);
}

static void buildTrialEvents(AFW& s) {
    s.trialEvents = s.macro;
    s.trialEvents[s.idx].frame += s.shift;
    std::stable_sort(s.trialEvents.begin(), s.trialEvents.end(),
                     [](InputEv const& a, InputEv const& b) { return a.frame < b.frame; });

    int shifted = s.macro[s.idx].frame + s.shift;
    int anchor  = shifted;
    if ((size_t)s.idx + 1 < s.macro.size()) anchor = std::max(anchor, s.macro[s.idx + 1].frame);
    s.trialEnd = std::min(anchor + cfgLookahead(), s.macroEnd);
}

// Pop practice checkpoints until the newest one is early enough for this trial.
// Returns false if the game removed the wrong checkpoint (flip the setting).
static bool prepareCheckpoints(PlayLayer* pl, AFW& s) {
    int shifted = s.macro[s.idx].frame + s.shift;
    int need    = std::min(shifted, s.macro[s.idx].frame) - cfgLead();

    int k = -1;
    for (size_t i = 0; i < s.cpFrames.size(); ++i)
        if (s.cpFrames[i] <= need) k = (int)i;

    if (k < 0) {
        if (!s.cpFrames.empty()) {
            pl->removeAllCheckpoints();
            s.cpFrames.clear(); s.cpObjs.clear();
        }
        return true;
    }
    while ((int)s.cpFrames.size() > k + 1) {
        pl->removeCheckpoint(cfgRemoveArg());
        s.cpFrames.pop_back(); s.cpObjs.pop_back();
        auto arr = pl->m_checkpointArray;
        if (!arr || arr->count() != s.cpFrames.size() || arr->lastObject() != s.cpObjs.back()) {
            log::error("removeCheckpoint removed the wrong checkpoint (count {} expected {})",
                       arr ? arr->count() : 0, s.cpFrames.size());
            return false;
        }
    }
    return true;
}

// After a reset: position the input cursor and re-press anything that was held
// at the checkpoint (practice checkpoints don't store hold state).
static void startSegment(PlayLayer* pl, AFW& s) {
    int startFrame = s.cpFrames.empty() ? 0 : s.cpFrames.back();
    auto& ev = (s.phase == Phase::Trials) ? s.trialEvents : s.macro;

    s.cursor = 0;
    while (s.cursor < ev.size() && ev[s.cursor].frame <= startFrame) s.cursor++;
    s.lastInjectFrame = -1;

    std::map<std::pair<int, bool>, bool> held;
    for (size_t i = 0; i < s.cursor; ++i) held[{ev[i].button, ev[i].p1}] = ev[i].down;

    s.injecting = true;
    for (auto const& [key, down] : held)
        if (down) pl->handleButton(true, key.first, key.second);
    s.injecting = false;
}

static void beginTrials(AFW& s) {
    s.results.assign(s.macro.size(), WindowResult{});
    s.idx = (int)s.macro.size() - 1;
    s.earlySide = true;
    s.shift = -1;
    s.pending = 0;
    s.phase = Phase::Trials;
    normalizeTrial(s);
}

static void finish(PlayLayer* pl, AFW& s) {
    s.phase = Phase::Idle;
    s.injecting = false;

    std::string csv = "index,frame,button,player1,down,early,late,window,early_capped,late_capped\n";
    int minWin = 1 << 30, le1 = 0, le2 = 0, le3 = 0;
    for (size_t i = 0; i < s.macro.size(); ++i) {
        auto const& e = s.macro[i]; auto const& r = s.results[i];
        int w = r.window();
        csv += fmt::format("{},{},{},{},{},{},{},{},{},{}\n", i, e.frame, e.button, (int)e.p1, (int)e.down,
                           r.early, r.late, w, (int)r.earlyCapped, (int)r.lateCapped);
        minWin = std::min(minWin, w);
        if (w <= 1) le1++;
        if (w <= 2) le2++;
        if (w <= 3) le3++;
    }

    int levelID = pl->m_level ? pl->m_level->m_levelID.value() : 0;
    auto path = Mod::get()->getSaveDir() / fmt::format("frame_windows_{}.csv", levelID);
    auto res = utils::file::writeString(path, csv);
    if (res.isErr()) log::error("could not write {}: {}", path.string(), res.unwrapErr());

    auto msg = fmt::format(
        "{} inputs, {} trials\nSmallest window: {} frame(s)\n<=1f: {}   <=2f: {}   <=3f: {}\n\nCSV: {}",
        s.macro.size(), s.trialsRun, minWin, le1, le2, le3, path.filename().string());
    FLAlertLayer::create("Frame Windows", msg, "OK")->show();
}

// ---------------------------------------------------------------- hooks
class $modify(AFWBase, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto& s = AFW::get();
        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this)
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (s.analysing()) {
            if (!s.injecting) return;               // drop real input while analysing
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (s.phase == Phase::Recording && !m_player1->m_isDead && !m_levelEndAnimationStarted)
            s.buffer.push_back({curFrame(this), down, button, isPlayer1});
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        auto& s = AFW::get();
        if (!s.analysing()) return;
        auto pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) return;

        if (s.pendingStart) {
            s.pendingStart = false;
            pl->resetLevelFromStart();
            return;
        }

        int frame = curFrame(this);
        if (frame == s.lastInjectFrame) return;     // several ticks can share a frame
        s.lastInjectFrame = frame;
        if (m_player1->m_isDead || m_levelEndAnimationStarted) return;

        auto& ev = (s.phase == Phase::Trials) ? s.trialEvents : s.macro;
        s.injecting = true;
        while (s.cursor < ev.size() && ev[s.cursor].frame + cfgInjectOff() <= frame) {
            auto const& e = ev[s.cursor];
            GJBaseGameLayer::handleButton(e.down, e.button, e.p1);
            s.cursor++;
        }
        s.injecting = false;

        if (s.phase == Phase::Laying) {
            if (frame >= s.macroEnd) {
                beginTrials(s);
                pl->resetLevel();
            }
        } else if (frame >= s.trialEnd) {
            s.pending = 2;
            pl->resetLevel();
        }
    }
};

class $modify(AFWPlay, PlayLayer) {
    struct Fields {
        CCLabelBMFont* hud = nullptr;
    };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        auto& s = AFW::get();
        s.resetAnalysis();
        s.buffer.clear();
        s.bufferDeathFrame = -1;
        int id = m_level ? m_level->m_levelID.value() : 0;
        if (id != s.lastLevelID) { s.lastRun.clear(); s.lastCompleted.clear(); s.lastLevelID = id; }
        s.phase = m_isPracticeMode ? Phase::Idle : Phase::Recording;

        auto lbl = CCLabelBMFont::create("", "bigFont.fnt");
        lbl->setScale(0.35f);
        lbl->setAnchorPoint({0.f, 1.f});
        auto win = CCDirector::sharedDirector()->getWinSize();
        lbl->setPosition({6.f, win.height - 6.f});
        lbl->setZOrder(1000);
        lbl->setID("afw-hud");
        if (m_uiLayer) m_uiLayer->addChild(lbl); else this->addChild(lbl);
        m_fields->hud = lbl;
    }

    void resetLevel() {
        auto& s = AFW::get();

        // ---- before the game resets
        if (s.phase == Phase::Laying && s.layingEndedByDeath) {
            s.layingEndedByDeath = false;
            beginTrials(s);
        }
        if (s.phase == Phase::Trials && s.pending) applyPendingResult(s);
        if (s.phase == Phase::Trials) {
            buildTrialEvents(s);
            if (!prepareCheckpoints(this, s)) {
                abortAnalysis("removeCheckpoint removed the wrong checkpoint. Flip the 'removeCheckpoint arg' setting and retry.");
            }
        }

        PlayLayer::resetLevel();

        // ---- after the game reset
        if (s.phase == Phase::Idle || s.phase == Phase::Recording) {
            if (!s.buffer.empty()) {
                s.lastRun = s.buffer;
                s.lastRunDeathFrame = s.bufferDeathFrame;
            }
            s.buffer.clear();
            s.bufferDeathFrame = -1;
            s.phase = m_isPracticeMode ? Phase::Idle : Phase::Recording;
            return;
        }
        if (s.analysing()) startSegment(this, s);
        if (s.phase == Phase::Done) finish(this, s);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        auto& s = AFW::get();
        if (object != m_anticheatSpike) {
            int frame = curFrame(this);
            switch (s.phase) {
                case Phase::Recording:
                    s.bufferDeathFrame = frame;
                    break;
                case Phase::Laying:
                    if (frame >= s.macroEnd - 2) s.layingEndedByDeath = true;
                    else abortAnalysis(fmt::format(
                        "replay desynced (died at frame {}). Random triggers or a non-deterministic level?", frame));
                    break;
                case Phase::Trials:
                    if (!s.pending) s.pending = 1;
                    break;
                default: break;
            }
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        auto& s = AFW::get();
        if (s.phase == Phase::Recording) {
            s.lastCompleted = s.buffer;
            s.lastRun = s.buffer;
            s.lastRunDeathFrame = -1;
            s.phase = Phase::Idle;
            Notification::create(fmt::format("Run captured: {} inputs. Pause -> Frame Windows.", s.buffer.size()),
                                 NotificationIcon::Success)->show();
            return PlayLayer::levelComplete();
        }
        if (s.analysing()) {
            // Reaching the end during analysis counts as surviving; don't run the end sequence.
            if (s.phase == Phase::Laying) { beginTrials(s); }
            else                          { s.pending = 2; }
            this->resetLevel();
            return;
        }
        PlayLayer::levelComplete();
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        auto& s = AFW::get();

        if (s.phase == Phase::Laying && !m_player1->m_isDead) {
            int frame = curFrame(this);
            if (frame > 0 && frame - s.lastCpFrame >= cfgCpInterval()) {
                if (auto cp = this->markCheckpoint()) {
                    s.lastCpFrame = frame;
                    s.cpFrames.push_back(frame);
                    s.cpObjs.push_back(cp);
                }
            }
        }

        if (auto hud = m_fields->hud) {
            std::string txt;
            switch (s.phase) {
                case Phase::Recording: txt = fmt::format("REC {} inputs", s.buffer.size()); break;
                case Phase::Laying:    txt = fmt::format("Replay pass  f{}  cp {}", curFrame(this), s.cpFrames.size()); break;
                case Phase::Trials: {
                    int done = (int)s.macro.size() - 1 - s.idx;
                    txt = fmt::format("Input {}/{}  shift {:+}  trials {}", done + 1, s.macro.size(), s.shift, s.trialsRun);
                    break;
                }
                default: break;
            }
            hud->setString(txt.c_str());
        }
    }

    void onQuit() {
        auto& s = AFW::get();
        s.resetAnalysis();
        s.phase = Phase::Idle;
        PlayLayer::onQuit();
    }
};

class $modify(AFWPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto& s = AFW::get();

        auto spr = ButtonSprite::create(s.analysing() ? "Stop FW" : "Frame Windows",
                                        "goldFont.fnt", "GJ_button_01.png", 0.5f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(AFWPause::onAnalyze));
        auto menu = CCMenu::create();
        menu->addChild(btn);
        auto win = CCDirector::sharedDirector()->getWinSize();
        menu->setPosition({win.width / 2.f, 30.f});
        menu->setID("afw-menu");
        this->addChild(menu, 10);
    }

    void onAnalyze(CCObject*) {
        auto& s = AFW::get();
        auto pl = PlayLayer::get();
        if (!pl) return;

        if (s.analysing()) {
            s.phase = Phase::Idle;
            s.injecting = false;
            this->onResume(nullptr);
            return;
        }

        if (pl->m_levelSettings && pl->m_levelSettings->m_platformerMode) {
            FLAlertLayer::create("Frame Windows", "Platformer levels aren't supported.", "OK")->show();
            return;
        }

        std::vector<InputEv> const* src = nullptr;
        int deathFrame = -1;
        if (!s.lastCompleted.empty())       { src = &s.lastCompleted; }
        else if (!s.lastRun.empty())        { src = &s.lastRun; deathFrame = s.lastRunDeathFrame; }
        else if (!s.buffer.empty())         { src = &s.buffer;  deathFrame = s.bufferDeathFrame; }
        if (!src) {
            FLAlertLayer::create("Frame Windows", "No inputs recorded yet. Play the level in normal mode first.", "OK")->show();
            return;
        }

        s.resetAnalysis();
        s.macro = *src;
        s.macroEnd = (deathFrame > 0) ? deathFrame - 1 : s.macro.back().frame + cfgLookahead();
        s.phase = Phase::Laying;
        s.pendingStart = true;

        if (!pl->m_isPracticeMode) pl->togglePracticeMode(true);
        pl->removeAllCheckpoints();

        log::info("analysis start: {} inputs, macroEnd {}", s.macro.size(), s.macroEnd);
        this->onResume(nullptr);
    }
};

class $modify(AFWScheduler, CCScheduler) {
    void update(float dt) {
        auto& s = AFW::get();
        if (s.analysing() && PlayLayer::get()) dt *= cfgSpeed();
        CCScheduler::update(dt);
    }
};
