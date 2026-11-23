// An add-on to start from. It takes no frames, so it needs no graphics code: it shows settings Unishade keeps, controls
// it draws itself, drawing over the game, what it is told of, and what it can ask of Unishade. unishade_addon.h says
// what everything is.
//
// Build it with the CMakeLists.txt beside it, then put the library and addon.ini in a folder of Unishade's Addons
// folder, which Add-ons > Open folder opens.

#include "unishade_addon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The settings, in the order their values arrive.
enum
{
    kGreeting,
    kTimes,
    kSayIt,
    kFrameRate,
    kSettingCount,
};

static const UnishadeSetting kSettings[kSettingCount] = {
    { .key = "Greeting", .label = "Greeting", .type = UNISHADE_SETTING_CHOICE, .choices = "Hello\0Welcome\0Good luck\0",
      .tooltip = "What the message says." },
    { .key = "Times", .label = "Times", .type = UNISHADE_SETTING_INTEGER, .defaultValue = { 1 }, .min = 1, .max = 3,
      .tooltip = "How many times it says it.", .group = "Message" },
    { .key = "SayIt", .label = "Say it", .type = UNISHADE_SETTING_BUTTON, .group = "Message" },
    { .key = "FrameRate", .label = "Show the frame rate", .type = UNISHADE_SETTING_TOGGLE, .tooltip = "In the top left corner of the game.",
      .group = "Over the game" },
};

typedef struct Example
{
    const UnishadeHost* host;
    char game[128];  // the game being played, or empty
    unsigned said;   // messages shown so far
    float shown;     // seconds the last message has left, for the bar
    double time;     // of the last update
    bool quiet;      // a value the add-on keeps itself, which its own switch sets
} Example;

static void* Create(const UnishadeHost* host, char* error, size_t errorSize)
{
    Example* example = calloc(1, sizeof(Example));
    if (!example)
    {
        snprintf(error, errorSize, "Out of memory.");
        return NULL;
    }
    example->host = host;
    host->log(host, UNISHADE_LOG_INFO, "Started.");
    return example;
}

static void Destroy(void* addon)
{
    free(addon);
}

static void Say(Example* example, int greeting, int times)
{
    static const char* const kGreetings[] = { "Hello", "Welcome", "Good luck" };
    char text[256] = "";
    for (int i = 0; i < times && strlen(text) + 160 < sizeof(text); ++i)
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s%s%s%s!", i ? " " : "", kGreetings[greeting], example->game[0] ? ", " : "",
                 example->game);
    example->host->showMessage(example->host, text);
    ++example->said;
    example->shown = 3;
}

// Once per pass of Unishade's main loop, with the settings' values. A button's is 1 in the first call after its press.
static bool Update(void* addon, const UnishadeUpdate* update, char* error, size_t errorSize)
{
    Example* example = addon;
    (void)error;
    (void)errorSize;
    if (example->time > 0 && example->shown > 0)
        example->shown -= (float)(update->time - example->time);
    example->time = update->time;
    if (update->settings[kSayIt][0] != 0 && !example->quiet)
        Say(example, (int)update->settings[kGreeting][0], (int)update->settings[kTimes][0]);
    return true;
}

static void Event(void* addon, const UnishadeEvent* event)
{
    Example* example = addon;
    switch (event->type)
    {
    case UNISHADE_EVENT_GAME_STARTED: snprintf(example->game, sizeof(example->game), "%s", event->text); break;
    case UNISHADE_EVENT_GAME_STOPPED: example->game[0] = '\0'; break;
    case UNISHADE_EVENT_SCREENSHOT_SAVED:
    {
        char line[512];
        snprintf(line, sizeof(line), "A screenshot went to %s.", event->text);
        example->host->log(example->host, UNISHADE_LOG_INFO, line);
        break;
    }
    default: break;
    }
}

// The add-on's own controls, under its settings, drawn again for every frame they show.
static void Ui(void* addon, const UnishadeUi* ui)
{
    Example* example = addon;
    const UnishadeHost* host = example->host;
    const UnishadeState* state = host->getState(host);
    char line[512];

    ui->heading(ui, "Now");
    if (state->playing)
        snprintf(line, sizeof(line), "Playing %s at %ux%u, %.0f frames a second, with %s.", state->gameName, state->width, state->height,
                 state->frameRate, state->presetName[0] ? state->presetName : "no preset");
    else
        snprintf(line, sizeof(line), "No game is being played. %u effects are loaded.", host->techniqueCount(host));
    ui->text(ui, line);
    snprintf(line, sizeof(line), "Said %u times", example->said);
    ui->progress(ui, line, example->shown > 0 ? example->shown / 3 : 0);

    ui->toggle(ui, "Quiet", &example->quiet);
    ui->tooltip(ui, "Say it does nothing while this is on. Unishade does not keep it: the add-on does, until it stops.");
    if (ui->button(ui, state->effectsOn ? "Turn effects off" : "Turn effects on"))
        host->setEffectsOn(host, !state->effectsOn);
    if (state->playing && ui->button(ui, "Take a screenshot"))
        host->takeScreenshot(host);
}

// Over the game, for every frame Unishade shows: the frame rate in a dark box, sized as Unishade's menu is.
static void Draw(void* addon, const UnishadeDraw* draw)
{
    const Example* example = addon;
    const UnishadeState* state = example->host->getState(example->host);
    if (draw->settings[kFrameRate][0] == 0)
        return;
    char text[32];
    snprintf(text, sizeof(text), "%.0f fps", state->frameRate);
    const float height = 16 * draw->scale, pad = 8 * draw->scale, edge = 12 * draw->scale;
    draw->rect(draw, edge, edge, draw->textWidth(draw, height, text) + pad * 2, height + pad * 2, UNISHADE_COLOR(0, 0, 0, 160), pad, 0);
    draw->text(draw, edge + pad, edge + pad, height, UNISHADE_COLOR(255, 255, 255, 255), text);
}

static const UnishadeAddon kAddon = {
    .size = sizeof(UnishadeAddon),
    .api = UNISHADE_API_VERSION,
    .settingCount = kSettingCount,
    .settings = kSettings,
    .settingSize = sizeof(UnishadeSetting),
    .create = Create,
    .destroy = Destroy,
    .update = Update,
    .event = Event,
    .ui = Ui,
    .draw = Draw,
};

UNISHADE_ADDON_EXPORT const UnishadeAddon* UnishadeGetAddon(void)
{
    return &kAddon;
}
