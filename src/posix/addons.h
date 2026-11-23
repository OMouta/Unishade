#pragma once

#include "effects.h"

#include <addon_manifest.h>
#include <addons/unishade_addon.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

class Surface;

// Add-ons, built against src/addons/unishade_addon.h. Each is a folder in the Addons folder of the data folder, with an
// addon.ini that says what it is, as src/addon_manifest.h describes, so nothing of an add-on runs before it is wanted.
// One whose library has the SHA-256 the official list has for its Id, src/addons/official.ini, loads without asking.
// Any other waits until the user says once whether to load it, which Unishade.ini keeps for its Id, so a rebuilt
// library loads without asking again.
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
        size_t count = 1; // values: three or four for a color
        float defaultValue[4] = {};
        std::vector<std::string> choices;
        // In the current preset, or in Unishade.ini for an add-on that leaves the picture alone. A button's stays 0.
        float value[4] = {};
    };

    struct Addon
    {
        std::string folder; // its folder's name in the Addons folder, which the functions below take
        // What its addon.ini says. Unishade.ini and presets name it by its Id. An add-on whose addon.ini cannot be
        // used has no Id, its folder's name as its name, and failed.
        addon::Manifest manifest;
        std::string icon; // its icon's file, or empty
        bool official = false;
        // Its Id is an official add-on's, and its library is not the one the official list has: an older or newer
        // one, which the Add-ons page offers to replace.
        bool replaceable = false;
        // Why it cannot be loaded at all, such as an addon.ini that cannot be used. It then failed, with this as error.
        std::string problem;
        State state = State::Pending;
        std::string error;
        // Enabled, but frames skip it. For an add-on that changes the picture, the preset says so: it is turned off
        // in this preset as an effect is.
        bool bypassed = false;
        // Enabled, but frames skip it while the picture is HDR, since it changes the picture and takes SDR ones only.
        // error says so meanwhile.
        bool hdrSkipped = false;
        std::vector<Setting> settings; // once it loaded
        std::shared_ptr<Library> library;
    };

    // An add-on on the official list that has a library for this system.
    struct Official
    {
        addon::Manifest manifest; // as this build has it
        std::string manifestText; // the addon.ini installing writes
        std::span<const unsigned char> icon; // the PNG installing writes as manifest.icon
        std::vector<addon::Download> files;
        std::string sha256; // of its library
    };

    Addons();
    ~Addons();
    void Init(fx::Runtime& runtime);
    // Stops every add-on. The graphics card must be idle.
    void Shutdown();

    // Reads the Addons folder again, for add-ons added, removed or replaced.
    void Scan();
    const std::vector<Addon>& List() const { return addons; }
    // Whether a loaded add-on changes the game's picture, rather than only reading it or writing depth.
    bool ChangesPicture(const Addon& addon) const;
    void SetBypassed(const std::string& folder, bool bypassed);
    // The answer for a Pending add-on: load it or not. Either way it is kept.
    void Approve(const std::string& folder, bool load);
    // Turning a failed add-on on tries it again.
    void SetEnabled(const std::string& folder, bool enabled);
    // Stops the add-on in a folder and lets go of its library until Resume, so its files can be installed again, from
    // the next Update on. Loaded says when it has.
    void Pause(const std::string& folder);
    // Reads the Addons folder again, and starts the add-on if it is on.
    void Resume(const std::string& folder);
    bool Loaded(const std::string& folder) const;
    // Kept in the current preset for an add-on that changes the picture, since it is part of the look, and in
    // Unishade.ini for any other, whose settings are about this computer. Takes as many values as the setting has. A
    // button is pressed by setting it, whatever the value.
    void SetSetting(const std::string& folder, size_t setting, const float* values);
    // The add-ons the preset says are on in it that are not running here: by name where an official add-on or one in
    // the Addons folder has the Id, otherwise by Id.
    std::vector<std::string> Missing() const;
    // Starts and stops add-ons as the calls above asked, takes settings from a preset that loaded, and calls the update
    // of every add-on that has one. Call once per pass of the main loop, outside drawing, since stopping one prepares the
    // effects again.
    void Update();

    // The official add-ons for this system.
    const std::vector<Official>& OfficialList() const { return official; }
    // The folder an official add-on is in, or the one installing it makes, named after its Id.
    std::string FolderOf(const Official& addon) const;
    // Downloads an official add-on's files into a folder of the Addons folder and checks each one's SHA-256, replacing
    // what is there, then writes its addon.ini and icon. Stop the add-on before installing it again. Blocks, so call it
    // from a thread of its own. Throws std::runtime_error with a message to show, or something else once cancel is
    // set. progress gets the file's index and its bytes so far.
    static void Install(const Official& addon, const std::string& folder, const std::atomic<bool>& cancel,
                        const std::function<void(size_t file, uint64_t received, uint64_t total)>& progress = {});

    // Has the add-ons that work on frames work on this one, in the overlay's frame being recorded, before effects and
    // after them, and renders the effects in between. Returns false when no add-on works on frames, or it could not
    // hand frames to them, and the caller renders as usual. While the picture is HDR, add-ons that do not take HDR
    // get an SDR copy, and those of them that change the picture sit out.
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
    // What Unishade.ini says about an add-on, by its Id: 1 to load it, 0 not to, -1 nothing yet.
    int Answer(const Addon& addon) const;
    void SetAnswer(const Addon& addon, bool load);
    Addon* Find(const std::string& folder);
    const Official* OfficialFor(const std::string& id) const;

    fx::Runtime* runtime = nullptr;
    std::vector<Addon> addons;
    std::vector<Official> official;
    std::set<std::string> paused;
    // Unishade.ini as it was when the first answer was asked for, and the answers read from it or given since, so
    // asking does not read the file again.
    mutable bool answersRead = false;
    mutable std::string answersText;
    mutable std::map<std::string, int> answers;
    std::unique_ptr<Exchange> exchange;
    uint64_t presetGeneration = 0;
    bool changed = false; // an answer changed since the last Update
    bool hdr = false; // the picture was HDR at the last Update, which says which add-ons sit out and provide DEPTH
};
