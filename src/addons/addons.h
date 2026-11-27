#pragma once

#include "effects/effects.h"

#include "addons/addon_manifest.h"
#include <unishade_addon.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class Surface;

// What add-ons ask of Unishade besides its effects, which the app answers.
class AddonHost
{
public:
    struct State
    {
        bool playing = false; // Unishade draws over a game
        bool effectsOn = true;
        bool menuOpen = false;
        float frameRate = 0;
        double time = 0; // seconds since Unishade started
        std::string gameName;
        std::string executable;
    };
    virtual State DescribeForAddons() = 0;
    virtual void ShowMessage(const std::string& text) = 0;
    virtual void SetEffectsOn(bool on) = 0;
    virtual void TakeScreenshot() = 0;
    // The presets the menu lists, the game's own first.
    virtual std::vector<fs::path> ListPresets() = 0;
    // False when the preset in use has changes to save or discard first.
    virtual bool UsePreset(const fs::path& preset) = 0;

protected:
    ~AddonHost() = default;
};

// Add-ons, built against addons/unishade_addon.h. Each is a folder in the Addons folder of the data folder, with an
// addon.ini that says what it is, as src/addons/addon_manifest.h describes, so nothing of an add-on runs before it is wanted.
// One whose library has the SHA-256 the official list has for its Id loads without asking. The official list is what
// the add-ons' own releases on GitHub say, as src/addons/official_addons.h describes. Any other add-on waits until
// the user says once whether to load it, which Unishade.ini keeps for its Id, so a rebuilt library, or another version
// of an official add-on, loads without asking again.
class Addons
{
public:
    struct Library;
    struct Exchange;

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
        std::string group; // the heading it goes under, or empty
        std::string units;
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
        // The SHA-256 of its library as it was last read, once the official list has its Id. Empty until then.
        std::string sha256;
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

    // An add-on on the official list: the release of it that this Unishade offers, which has a library for this system.
    struct Official
    {
        std::string tag;          // of the release, such as unishade.depth-v1.2.0
        bool beta = false;        // the release is a beta, whose files are not signed
        addon::Manifest manifest; // as the release has it
        std::string manifestText; // the addon.ini installing writes
        std::string icon;         // the PNG installing writes as manifest.icon, or empty
        std::string picture;      // the file Unishade keeps that PNG in, for the Add-ons page, or empty
        std::vector<addon::Download> files;
        std::string sha256; // of its library
    };

    // A control an add-on draws under its settings, as UnishadeUi describes them.
    struct Control
    {
        enum class Kind
        {
            Heading,
            Text,
            Progress,
            Button,
            Toggle,
            Slider,
            Integer,
            Choice,
            Color,
        };
        Kind kind = Kind::Text;
        std::string label; // without what follows ##
        std::string id;    // with it, which tells the add-on's controls apart
        std::string tooltip;
        float min = 0;
        float max = 1;
        size_t count = 1;
        float value[4] = {};
        std::vector<std::string> choices;
    };

    Addons();
    ~Addons();
    void Init(fx::Runtime& runtime, AddonHost& host);
    // Stops every add-on. The graphics card must be idle.
    void Shutdown();

    // Reads the Addons folder again, for add-ons added, removed or replaced.
    void Scan();
    const std::vector<Addon>& List() const { return addons; }
    // Whether a loaded add-on changes the game's picture, rather than only reading it or writing depth.
    bool ChangesPicture(const Addon& addon) const;
    // Whether a loaded add-on draws controls of its own.
    bool HasControls(const Addon& addon) const;
    // Has an add-on that is on draw its controls, and returns them, which stay until the next call for it. Call it
    // for every frame they show.
    const std::vector<Control>& Controls(const std::string& folder);
    // What the user did with one of them, by its place among them: its new values, or a press. The add-on's next
    // drawing tells it.
    void UseControl(const std::string& folder, size_t control, const float* values);
    // Has every add-on that is on, and not bypassed, draw over the game with what the caller made: its size, the
    // picture's and the functions that draw. Each add-on gets its settings and the time in it.
    void Draw(UnishadeDraw& draw);
    // Whether an add-on that is on wants the keys read, for UnishadeHost.keyDown.
    bool ReadsKeys() const;
    // Tells every add-on that is on of something that happened.
    void Tell(UnishadeEventType type, uint32_t value = 0, const std::string& text = {});
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
    // button is pressed by setting it, whatever the value. The add-on is told of the change unless it made it itself.
    void SetSetting(const std::string& folder, size_t setting, const float* values, bool tell = true);
    // The add-ons the preset says are on in it that are not running here: by name where an official add-on or one in
    // the Addons folder has the Id, otherwise by Id.
    std::vector<std::string> Missing() const;
    // Starts and stops add-ons as the calls above asked, takes settings from a preset that loaded, tells add-ons what
    // happened since the last call, and calls the update of every add-on that has one. Call once per pass of the main
    // loop, outside drawing, since stopping one prepares the effects again.
    void Update();

    // The official add-ons for this system, as the last look found them. Until the first one of all, there are none.
    const std::vector<Official>& OfficialList() const { return official; }
    // Asks GitHub which official add-ons there are, on a thread of its own, unless one is asking or one asked a short
    // while ago. Call it while the Add-ons page shows. The next Update after the answer takes it, and wake is called
    // from the thread once there is one, so the caller can have that Update run. Tests give their own fetch.
    void LookForOfficial(const std::function<void()>& wake, const std::function<std::string(const std::string& url)>& fetch = {});
    bool LookingForOfficial() const { return static_cast<bool>(look); }
    // Whether the last look could not ask GitHub, so the list is the one from before it.
    bool OfficialLookFailed() const { return lookFailed; }
    // Goes up when a look ended, whether the list changed or not.
    unsigned OfficialGeneration() const { return officialGeneration; }
    // The folder an official add-on is in, or the one installing it makes, named after its Id.
    std::string FolderOf(const Official& addon) const;
    // Downloads an official add-on's files into a folder of the Addons folder and checks each one's SHA-256, replacing
    // what is there, then writes its addon.ini and icon. On Windows the library of a version, which is not a beta, also
    // has to be signed as net/update.h says of a release's files. Stop the add-on before installing it again. Blocks,
    // so call it from a thread of its own. Throws std::runtime_error with a message to show, or something else once
    // cancel is set. progress gets the file's index and its bytes so far.
    static void Install(const Official& addon, const std::string& folder, const std::atomic<bool>& cancel,
                        const std::function<void(size_t file, uint64_t received, uint64_t total)>& progress = {});

    // Has the add-ons that work on frames work on this one, in the overlay's frame being recorded, before effects and
    // after them, and renders the effects in between. Returns false when no add-on works on frames, or it could not
    // hand frames to them, and the caller renders as usual. While the picture is HDR, add-ons that do not take HDR
    // get an SDR copy, and those of them that change the picture sit out.
    bool Frame(Surface& surface, const fx::Runtime::Source& source, uint64_t picture);

private:
    void Load(Addon& addon);
    void Tell(Addon& addon, UnishadeEventType type, uint32_t value, const std::string& text);
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
    // Says whether an add-on is the one the official list has for its Id, or another build of it.
    void Mark(Addon& addon);
    // Takes the answer of a look that ended.
    void TakeOfficial();

    fx::Runtime* runtime = nullptr;
    AddonHost* host = nullptr;
    // What add-ons were last told of.
    AddonHost::State told;
    uint64_t effectsGeneration = 0;
    std::vector<Addon> addons;
    std::vector<Official> official;
    // A look that is asking GitHub, or whose answer the next Update takes.
    struct Look;
    std::shared_ptr<Look> look;
    std::chrono::steady_clock::time_point lookAgain{};
    bool lookFailed = false;
    unsigned officialGeneration = 0;
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
