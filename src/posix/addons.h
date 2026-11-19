#pragma once

#include "effects.h"

#include <addons/unishade_addon.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

class Surface;

// Add-ons, built against src/addons/unishade_addon.h, from the Addons folder in the data folder. One whose SHA-256 is
// on the official list, src/addons/official.ini, loads without asking. Any other waits until the user says once
// whether to load it, which Unishade.ini keeps for its path, so a rebuilt file loads without asking again.
class Addons
{
    struct Library;
    struct Exchange;

public:
    enum class State
    {
        Pending, // not official, and the user has not said whether to load it
        Enabled,
        Disabled,
        Failed, // error says why
    };

    struct Setting
    {
        std::string key;
        std::string label;
        std::string tooltip;
        UnishadeSettingType type = UNISHADE_SETTING_TOGGLE;
        float min = 0;
        float max = 1;
        float defaultValue = 0;
        std::vector<std::string> choices;
        float value = 0; // in the current preset, or in Unishade.ini for an add-on that leaves the picture alone
    };

    struct Addon
    {
        std::string file; // its path in the Addons folder, which Unishade.ini remembers
        std::string name; // its own once it loaded, before that the official list's or the file's
        std::string description;
        bool official = false;
        State state = State::Pending;
        std::string error;
        // Enabled, but frames skip it. For an add-on that changes the picture, the preset says so: it is turned off
        // in this preset as an effect is.
        bool bypassed = false;
        std::vector<Setting> settings; // once it loaded
        std::shared_ptr<Library> library;
    };

    // An add-on on the official list.
    struct Official
    {
        struct File
        {
            std::string path; // in the Addons folder
            std::string sha256;
            std::string url;
        };
        std::string name;
        std::string description;
        std::vector<File> files; // the first is the add-on itself
    };

    Addons();
    ~Addons();
    void Init(fx::Runtime& runtime);
    // Stops every add-on. The graphics card must be idle.
    void Shutdown();

    // Reads the Addons folder again, for files added, removed or replaced.
    void Scan();
    const std::vector<Addon>& List() const { return addons; }
    // Whether a loaded add-on changes the game's picture, rather than only reading it or writing depth.
    bool ChangesPicture(const Addon& addon) const;
    void SetBypassed(const std::string& file, bool bypassed);
    // The answer for a Pending add-on: load it or not. Either way it is kept.
    void Approve(const std::string& file, bool load);
    // Turning a failed add-on on tries it again.
    void SetEnabled(const std::string& file, bool enabled);
    // Stops an add-on and lets go of its library until Resume, so its files can be installed again, from the next
    // Update on. Loaded says when it has.
    void Pause(const std::string& file);
    // Reads the Addons folder again, and starts the add-on if it is on.
    void Resume(const std::string& file);
    bool Loaded(const std::string& file) const;
    // Kept in the current preset for an add-on that changes the picture, since it is part of the look, and in
    // Unishade.ini for any other, whose settings are about this computer.
    void SetSetting(const std::string& file, size_t setting, float value);
    // The add-ons the preset says are on in it that are not running here, by name.
    std::vector<std::string> Missing() const;
    // Starts and stops add-ons as the calls above asked, and takes settings from a preset that loaded. Call once per
    // pass of the main loop, outside drawing, since stopping one prepares the effects again.
    void Update();

    // The official add-ons for this system.
    const std::vector<Official>& OfficialList() const { return official; }
    // Downloads an official add-on's files into the Addons folder and checks each one's SHA-256, replacing what is
    // there. Stop the add-on before installing it again. Blocks, so call it from a thread of its own. Throws
    // std::runtime_error with a message to show, or on Windows net.h's Cancelled once cancel is set. progress gets the
    // file's index and its bytes so far.
    static void Install(const Official& addon, const std::atomic<bool>& cancel,
                        const std::function<void(size_t file, uint64_t received, uint64_t total)>& progress = {});

    // Has the add-ons that work on frames work on this one, in the overlay's frame being recorded, before effects.
    // Copies the picture in itself, so the caller goes on with RenderEffects. Returns false, having done nothing, when
    // no add-on works on frames, and the caller renders as usual.
    bool Frame(Surface& surface, const fx::Runtime::Source& source, uint64_t picture);

private:
    void Load(Addon& addon);
    void Start(Addon& addon);
    void Stop(Addon& addon, bool unload);
    void Fail(Addon& addon, const std::string& error);
    void ReadSettings(Addon& addon);
    // Writes whether the add-on is on in the preset, and every one of its values, so the preset looks the same
    // wherever the add-on runs and whatever its defaults become.
    void WritePreset(const Addon& addon);
    // What Unishade.ini says about a file: 1 to load it, 0 not to, -1 nothing yet.
    int Answer(const std::string& file) const;
    void SetAnswer(const std::string& file, bool load);
    Addon* Find(const std::string& file);

    fx::Runtime* runtime = nullptr;
    std::vector<Addon> addons;
    std::vector<Official> official;
    std::set<std::string> paused;
    std::unique_ptr<Exchange> exchange;
    uint64_t presetGeneration = 0;
    bool changed = false; // an answer changed since the last Update
};
