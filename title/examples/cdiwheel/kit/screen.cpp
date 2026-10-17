// ps5-homebrew-ui - Design "Cover Wheel": a carousel of covers you can spin.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Seedee5's copy of PS5_VKHomebrewUI's src/concepts/carousel.cpp, in a namespace of its own (hui::screen),
// drawn by the program (../cdiwheel.cpp) through ps5ui::make_screen, below. Seedee5's changes to the kit's
// design:
//   - the wheel holds the CD-i games of the game list (cdi::library(), built by the program from the
//     engine), however many there are, and follows the list when it changes (the NAS arrives, a rescan);
//   - covers keep their own shape inside the square (a CD jewel case, a long box), standing on the floor;
//     a game without a picture yet is a dark card with its title on it;
//   - L1 and R1 jump to the first game of the previous or next letter, with a big letter to show where
//     the wheel landed; L2 and R2 jump ten games;
//   - the caption and the details say what a CD-i game has: its region and other tags, where it is (on the
//     console, a NAS), how many discs, the file format; Play starts it (a NAS game is copied first);
//   - favourites are kept (the program saves them); Square looks for games again;
//   - the header shows the NAS and the cover downloads while they have something to say.
//
// The whole collection stands in one row on a dark, reflecting floor. The
// carousel is not a list with an index: it is a wheel with a position and a
// velocity, and everything on screen is drawn from that one number. What
// makes it feel finished:
//
//   - the left stick scrubs the wheel, releasing it lets it coast on its own
//     inertia, and a spring then pulls it onto the nearest cover; the D-pad
//     and the triggers only move the spring's target, so every way of moving
//     blends into every other;
//   - depth without 3D: a cover's place, size and brightness are smooth
//     functions of its distance from the wheel position, and covers are drawn
//     far to near so the near ones overlap the far ones;
//   - each cover is mirrored in the floor: the same texture with its v range
//     reversed, squashed, darkened and veiled by a gradient of the floor's
//     own colour;
//   - the two ends are rubber: scrubbing into one stretches a little, coasting
//     into one bounces back, and pressing against one gives a soft refusal;
//   - the caption follows the wheel but only shows once it settles, so fast
//     scrubbing blurs past titles instead of strobing them;
//   - Cross lifts the centred cover out of the row toward the player while
//     the rest slide away, and a frosted panel with its details fades in;
//   - the open card shows what the game is (from its Wikipedia article), its
//     review scores and its logo, over a dimmed screenshot of it.

#include "concepts/concepts.hpp"

#include "cdi_library.hpp"

#include "core/tween.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace hui::screen
{

namespace
{

using gfx::Color;
using gfx::Rect;

const Color kWhite = Color::rgb(0xffffff);
const Color kClear = Color::rgb(0x000000, 0.0f);
const Color kBlack = Color::rgb(0x000000);
const Color kNight = Color::rgb(0x030408); // what every dark tone is mixed toward
const Color kInk = Color::rgb(0x0b0d16);   // text on white buttons
const Color kGold = Color::rgb(0xffd166);
const Color kWarn = Color::rgb(0xf2a65a);
const Color kGood = Color::rgb(0x7fd59a);

// ---- the row: where a cover sits at distance d from the wheel position ----
constexpr float kCentreX = 960.0f;
constexpr float kCover = 440.0f;   // size of the centred cover
constexpr float kRadius = 28.0f;   // its corner radius; scales with the cover
constexpr float kBaseY = 592.0f;   // the floor line under the centred cover
constexpr float kVanishY = 516.0f; // far covers stand closer to this height
constexpr float kSpreadA = 560.0f; // x = A * (1 - exp(-k |d|)) + B * |d|: the first
constexpr float kSpreadK = 0.95f;  //   step is wide, later ones crowd together
constexpr float kSpreadB = 40.0f;
constexpr float kFarScale = 0.36f; // size of the farthest covers
constexpr float kScaleFall = 1.1f;
constexpr float kFarLight = 0.26f; // brightness of the farthest covers
constexpr float kLightFall = 0.8f;
constexpr float kSoftAbs = 0.3f; // rounds |d| at 0 so size has no sharp peak
constexpr float kReach = 6.0f;   // covers farther than this are not drawn

// ---- the floor and the reflections ----
constexpr float kFloorY = 536.0f;      // the floor is opaque from here down
constexpr float kHorizon = 120.0f;     // ... and fades in over this height
constexpr float kReflectPart = 0.42f;  // how much of a cover is mirrored
constexpr float kReflectSquash = 0.8f; // the mirror image is a little flatter
constexpr float kReflectLight = 0.3f;  // and much darker
constexpr float kReflectHaze = 0.18f;  // veil opacity right under the cover

// ---- the wheel: units are covers and seconds ----
constexpr int kJump = 10;              // L2 / R2
constexpr float kStickDead = 0.12f;    // below this the stick does not scrub
constexpr float kScrubSpeed = 11.0f;   // covers per second at full deflection
constexpr float kScrubResponse = 5.0f; // how quickly the wheel takes the stick's speed
constexpr float kFriction = 1.8f;      // coasting: speed decays by e every 1/1.8 s
constexpr float kSnapSpeed = 1.4f;     // below this a coasting wheel is caught
constexpr float kSnapLook = 0.18f;     // ... on the cover it would reach in this time
constexpr float kSnapOmega = 13.0f;
constexpr float kSnapDamping = 0.8f; // a touch under critical: weight, not wobble
constexpr float kRubber = 0.3f;      // how far a held stick stretches past an end
constexpr float kRubberRate = 12.0f;
constexpr float kRefuseKick = 3.2f;     // speed given to the wheel by a refused press
constexpr float kMaxEndSpeed = 8.0f;    // a coasting wheel hits an end no faster
constexpr float kEndSlack = 0.02f;      // this far past an end counts as hitting it
constexpr float kTickInterval = 0.085f; // at most about twelve ticks a second

// ---- the caption and the scrub bar ----
constexpr float kHeaderY = 94.0f;
constexpr float kTitleY = 808.0f;
constexpr float kMetaY = 856.0f;
constexpr float kBarX = 560.0f;
constexpr float kBarW = 800.0f;
constexpr float kBarY = 918.0f;
constexpr float kCalmFrom = 5.5f; // faster than this the caption starts to hide
constexpr float kCalmSpan = 2.5f;
constexpr int kMaxDots = 160;     // more games than this: the bar shows letters instead of a dot each

// ---- the opened cover and its panel ----
const Rect kOpenRect{176.0f, 280.0f, 520.0f, 520.0f};
const Rect kPanel{760.0f, 116.0f, 1064.0f, 848.0f};
constexpr float kPanelRadius = 40.0f;
constexpr float kPanelPad = 56.0f;
constexpr float kOpenSlide = 520.0f; // how far the other covers move away

constexpr const char *kTechniques[] = {
    "A wheel with position and velocity: stick scrubbing, inertia, spring snap to a cover",
    "Depth without 3D: place, size and light from distance; covers drawn far to near",
    "Floor reflections: the cover texture with reversed v, squashed and veiled by a gradient",
    "Rubber-band ends: stretch while scrubbing, bounce when coasting, soft refusal on a press",
    "A caption that waits for the wheel to settle, so fast scrubbing never strobes text",
    "The centred cover lifts out of the row beside a frosted details panel (Frame::glass)",
};

// How the wheel is being driven right now.
enum class Drive
{
    snap,  // a spring pulls it onto target_
    scrub, // the stick sets its speed
    coast, // nothing touches it: friction only
};

// Where one cover is drawn.
struct Pose
{
    Rect rect;
    float light = 1.0f; // brightness of the artwork
    float alpha = 1.0f;
    float reflect = 1.0f; // strength of its mirror image
};

const cdi::Game kNoGame{};

class CoverFlow final : public app::Concept
{
  public:
    explicit CoverFlow(app::Context &context) : context_(context)
    {
        calm_.snap(1.0f);
        sync(true);
        apply_palette(true);
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "cdiwheel",
            "Seedee5",
            "Your CD-i games on a wheel: scrub it, let it coast, open a cover, play",
            "examples/cdiwheel/kit/screen.cpp",
            audio::SoundSet::glass,
            Color::rgb(0x38e8ff),
            kTechniques,
        };
        return kInfo;
    }

    void enter() override
    {
        age_ = 0.0f;
        open_ = false;
        launch_.running = false;
        // Coming back mid-spin would be odd: the wheel is at rest on a cover.
        settle_on(current());
        quiet_ = true;
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        age_ += dt;
        dt_ = dt;
        clock_ += dt;
        tick_wait_ = std::max(0.0f, tick_wait_ - dt);
        letter_shown_ = std::max(0.0f, letter_shown_ - dt);
        sync(false);
        // The game that was started has ended: the details panel goes, the row comes back.
        if (was_playing_ && !cdi::playing())
        {
            open_ = false;
            launch_.running = false;
            age_ = 0.0f;
        }
        was_playing_ = cdi::playing();
        if (count_ > 0)
        {
            if (open_)
                update_details(input, feedback);
            else
                update_wheel(input, feedback);
            spin(dt, feedback);
        }
        else if (input.is_pressed(Action::west))
        {
            cdi::rescanRequest() = true;
            feedback.play(audio::Cue::select);
        }
        cdi::centre() = count_ > 0 ? nearest() : 0;

        // ---- animation state ----
        const float speed = std::fabs(wheel_.velocity);
        calm_.target = 1.0f - tween::clamp01((speed - kCalmFrom) / kCalmSpan);
        calm_.update(dt, 14.0f);
        for (ui::SpringColor &colour : palette_)
            colour.update(dt, 4.5f);
        lift_.target = open_ ? 1.0f : 0.0f;
        lift_.update(dt, reduced() ? 40.0f : 11.0f);
        cdi::openCard() = count_ > 0 && (open_ || lift_.value > 0.02f) ? opened_ : -1;
        // the open game's screenshot fades in behind it when it arrives
        backdrop_.target = cdi::openCard() >= 0 && item(opened_).backdrop ? 1.0f : 0.0f;
        backdrop_.update(dt, reduced() ? 40.0f : 3.0f);
        launch_.update(dt);
        star_.update(dt, 5.0f);
    }

    void draw(app::Frame &frame) const override
    {
        // A spotlight in the centred cover's body colour, behind the row.
        frame.backdrop.mode = gfx::BackdropMode::gradient;
        frame.backdrop.colors[0] = palette_[0].value();
        frame.backdrop.colors[1] = palette_[1].value();
        frame.backdrop.colors[2] = palette_[2].value();
        frame.backdrop.params[0] = 0.5f;
        frame.backdrop.params[1] = 0.34f;
        frame.backdrop.params[2] = 0.5f + (reduced() ? 0.0f : 0.06f * ui::breathe(clock_, 5.0f));
        frame.backdrop.time = clock_;

        gfx::DrawList &list = frame.scene;
        const float lift = tween::clamp01(lift_.value);
        draw_floor(list);
        draw_header(list);
        if (count_ == 0)
        {
            draw_empty(list);
            return;
        }
        draw_row(list, lift);
        draw_caption(list, lift);
        draw_scrub_bar(list, lift);
        draw_letter(list, lift);
        if (lift > 0.01f)
        {
            // The row steps back: a scrim over everything but the lifted cover, and behind it, once it has
            // arrived, a screenshot of the game filling the screen.
            draw_backdrop(list, lift);
            list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                              kNight.with_alpha(0.42f * lift));
            // Launching: the cover flares in its own accent and the light
            // washes over the whole screen for a moment.
            const float flare = launch_.running ? 1.0f - launch_.progress() : 0.0f;
            draw_cover(list, opened_, pose(opened_), lift * (1.0f + 1.6f * flare * flare));
            frame.glass = true;
            draw_details(frame.overlay, frame.glass_texture, lift);
            if (flare > 0.0f)
                frame.overlay.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0,
                                           gfx::mix(item(opened_).accent, kWhite, 0.55f)
                                               .with_alpha(0.26f * lift * flare * flare * flare));
        }
        draw_hints(lift > 0.5f ? frame.overlay : frame.scene, lift);
    }

  private:
    bool reduced() const
    {
        return context_.settings.reduced_motion;
    }

    const cdi::Game &item(int index) const
    {
        const std::vector<cdi::Game> &games = cdi::library().games;
        if (index < 0 || index >= static_cast<int>(games.size()))
            return kNoGame;
        return games[static_cast<std::size_t>(index)];
    }

    float last() const
    {
        return static_cast<float>(std::max(0, count_ - 1));
    }

    // Follows the list: its length, and a jump the program asks for (a rescan kept the selected game
    // somewhere else, or the list opened on the game played last).
    void sync(bool first)
    {
        const int count = static_cast<int>(cdi::library().games.size());
        if (count != count_)
        {
            count_ = count;
            if (count_ > 0)
            {
                target_ = std::clamp(target_, 0, count_ - 1);
                centre_ = std::clamp(centre_, 0, count_ - 1);
                opened_ = std::clamp(opened_, 0, count_ - 1);
                wheel_.value = std::clamp(wheel_.value, 0.0f, last());
            }
            if (count_ == 0)
                open_ = false;
        }
        int &jump = cdi::jumpTo();
        if (jump >= 0 && count_ > 0)
        {
            target_ = centre_ = opened_ = std::clamp(jump, 0, count_ - 1);
            wheel_.snap(static_cast<float>(target_));
            drive_ = Drive::snap;
            quiet_ = true;
            apply_palette(first);
            jump = -1;
        }
    }

    // The cover closest to the wheel position.
    int nearest() const
    {
        return std::clamp(static_cast<int>(std::lround(wheel_.value)), 0, std::max(0, count_ - 1));
    }

    // The cover the player means: where the wheel is going, or where it is.
    int current() const
    {
        return drive_ == Drive::snap ? std::clamp(target_, 0, std::max(0, count_ - 1)) : nearest();
    }

    // How far the wheel is past an end: negative before the first cover,
    // positive after the last, zero in between.
    float beyond() const
    {
        if (wheel_.value < 0.0f)
            return wheel_.value;
        return std::max(0.0f, wheel_.value - last());
    }

    // The stick is scrubbing outward from the first or the last cover.
    bool pushing_at_end() const
    {
        return drive_ == Drive::scrub && ((wheel_.value <= 0.0f && stick_ < 0.0f) ||
                                          (wheel_.value >= last() && stick_ > 0.0f));
    }

    // 1 when the wheel rests on a cover, 0 half-way between two. The caption,
    // the focus ring and the glow all fade with it. A wheel stretched past an
    // end still counts as resting on the end cover.
    float settled() const
    {
        const float inside = std::clamp(wheel_.value, 0.0f, last());
        const float off = std::fabs(inside - static_cast<float>(centre_));
        return 1.0f - tween::smoothstep((off - 0.08f) / 0.34f);
    }

    void settle_on(int index)
    {
        target_ = std::clamp(index, 0, std::max(0, count_ - 1));
        drive_ = Drive::snap;
    }

    // The backdrop, the floor and the accents take the centred cover's colours.
    void apply_palette(bool snap)
    {
        const cdi::Game &it = item(centre_);
        const Color sky = gfx::mix(it.dark, it.mid, 0.3f);
        const Color ground = gfx::mix(it.dark, it.mid, 0.12f);
        const Color targets[5] = {
            gfx::mix(it.dark, kNight, 0.6f), // backdrop, top
            gfx::mix(sky, kNight, 0.35f),    // backdrop, bottom
            it.mid,                          // the spotlight behind the row
            gfx::mix(ground, kNight, 0.5f),  // the floor at its horizon
            it.accent,                       // the scrub bar's thumb
        };
        for (int i = 0; i < 5; ++i)
        {
            if (snap)
                palette_[i].snap(targets[i]);
            else
                palette_[i].target(targets[i]);
        }
    }

    // ---- input ----

    void update_wheel(const InputFrame &input, app::Feedback &feedback)
    {
        // The stick also produces nav repeats; while it scrubs, those are not
        // steps. A D-pad press arrives with the stick at rest.
        const bool scrubbing = std::fabs(input.stick_x) >= kStickDead && !input.focus_lost;
        if (scrubbing)
        {
            drive_ = Drive::scrub;
            stick_ = input.stick_x;
            quiet_ = false;
        }
        else if (drive_ == Drive::scrub)
        {
            release();
        }

        if (!scrubbing && (input.nav == Direction::left || input.nav == Direction::right))
        {
            const int direction = input.nav == Direction::right ? 1 : -1;
            if (move_target(direction))
            {
                // Left and right sound left and right, and a hair apart in pitch.
                feedback.play(audio::Cue::focus, 1.0f + 0.025f * static_cast<float>(direction),
                              0.3f * static_cast<float>(direction));
            }
            else if (!input.nav_repeat)
            {
                refuse(direction, feedback);
            }
        }
        for (const int direction : {-1, 1})
        {
            if (!input.is_pressed(direction < 0 ? Action::jump_prev : Action::jump_next))
                continue;
            if (move_target(direction * kJump))
                feedback.play(audio::Cue::tab, 1.0f + 0.06f * static_cast<float>(direction),
                              0.4f * static_cast<float>(direction));
            else
                refuse(direction, feedback);
        }
        // L1 / R1: the first game of the previous or next letter; held, it keeps stepping letter by letter
        for (const int direction : {-1, 1})
        {
            const Action a = direction < 0 ? Action::page_prev : Action::page_next;
            float &held = letter_held_[direction < 0 ? 0 : 1];
            bool step = input.is_pressed(a);
            if (step)
                held = 0.0f;
            else if (input.is_held(a))
            {
                const float before = held;
                held += dt_;
                // after 0.45 s, a letter every 0.18 s
                step = held >= 0.45f && std::floor((held - 0.45f) / 0.18f) != std::floor((before - 0.45f) / 0.18f);
            }
            else
                held = 0.0f;
            if (!step)
                continue;
            const int to = letter_step(current(), direction);
            if (to != current() && move_target(to - current()))
            {
                feedback.play(audio::Cue::tab, 1.0f + 0.04f * static_cast<float>(direction),
                              0.4f * static_cast<float>(direction));
                letter_shown_ = 1.1f;
                letter_index_ = to;
            }
            else if (input.is_pressed(a))
            {
                refuse(direction, feedback); // (held at the first or last letter: quietly stays)
            }
        }
        if (input.is_pressed(Action::north))
            toggle_favorite(feedback);
        if (input.is_pressed(Action::west))
        {
            cdi::rescanRequest() = true;
            feedback.play(audio::Cue::select);
        }
        if (input.is_pressed(Action::confirm))
        {
            opened_ = current();
            settle_on(opened_);
            quiet_ = true;
            open_ = true;
            feedback.play(audio::Cue::open);
        }
    }

    void update_details(const InputFrame &input, app::Feedback &feedback)
    {
        if (cdi::playing())
            return; // the program has the screen until the game ends
        // Cross: Continue when the game has an automatic save, else Play; Square: start it from the beginning
        const bool canContinue = !item(opened_).resume.empty();
        const bool cross = input.is_pressed(Action::confirm);
        const bool fresh = canContinue && input.is_pressed(Action::west);
        if ((cross || fresh) && !launch_.running)
        {
            feedback.play(audio::Cue::launch);
            feedback.rumble(0.7f, 0.18f);
            launch_.start(0.9f);
            cdi::playRequest() = opened_;
            cdi::continueRequest() = cross && canContinue;
        }
        if (input.is_pressed(Action::north))
            toggle_favorite(feedback);
        if (input.is_pressed(Action::back))
        {
            feedback.play(audio::Cue::back);
            open_ = false;
        }
    }

    // The letter group a game belongs to: 0 for '#', 1..26 for A..Z
    static int bucket(char letter)
    {
        return letter >= 'A' && letter <= 'Z' ? letter - 'A' + 1 : 0;
    }

    // From `from`, the first game of the next letter (direction 1), or of this letter and then the one before it
    // (direction -1). The list is in name order, so a letter's games are together.
    int letter_step(int from, int direction) const
    {
        const int b = bucket(item(from).letter);
        if (direction > 0)
        {
            int to = from;
            while (to < count_ && bucket(item(to).letter) == b)
                ++to;
            return to < count_ ? to : from;
        }
        int start = from;
        while (start > 0 && bucket(item(start - 1).letter) == b)
            --start;
        if (start < from)
            return start;
        if (start == 0)
            return from;
        int to = start - 1;
        const int pb = bucket(item(to).letter);
        while (to > 0 && bucket(item(to - 1).letter) == pb)
            --to;
        return to;
    }

    // Moves the spring's target by `delta` covers, stopping at the ends.
    // Returns false when it could not move at all.
    bool move_target(int delta)
    {
        const int from = current();
        const int to = std::clamp(from + delta, 0, count_ - 1);
        if (to == from)
            return false;
        settle_on(to);
        quiet_ = true; // the press had its own sound: no ticks on the way
        return true;
    }

    // The end of the row answers: a quiet cue, a light pulse, and the wheel
    // is pushed against its stop so the spring shows the bump.
    void refuse(int direction, app::Feedback &feedback)
    {
        feedback.play(audio::Cue::error, 1.0f, 0.4f * static_cast<float>(direction), 0.6f);
        feedback.rumble(0.25f, 0.05f);
        quiet_ = true;
        if (reduced())
            return;
        settle_on(current());
        wheel_.kick(kRefuseKick * static_cast<float>(direction));
    }

    // The stick was let go.
    void release()
    {
        if (beyond() != 0.0f)
        {
            settle_on(nearest());
        }
        else if (reduced() || std::fabs(wheel_.velocity) < kSnapSpeed)
        {
            catch_wheel();
        }
        else
        {
            drive_ = Drive::coast;
        }
    }

    // Hands a slow wheel to the spring, aimed at the cover it was heading for.
    // The velocity is kept, so the hand-over cannot be seen.
    void catch_wheel()
    {
        settle_on(static_cast<int>(std::lround(wheel_.value + wheel_.velocity * kSnapLook)));
    }

    void toggle_favorite(app::Feedback &feedback)
    {
        const int index = open_ ? opened_ : current();
        if (index < 0 || index >= count_)
            return;
        const bool on = !item(index).favorite;
        cdi::favouriteToggles().push_back(index); // the program changes and saves it
        feedback.play(on ? audio::Cue::favorite_on : audio::Cue::favorite_off);
        if (on)
            star_.trigger();
    }

    // ---- physics ----

    // Advances the wheel by dt according to how it is driven, then reports
    // what the movement caused: an end reached, a new cover in the centre.
    void spin(float dt, app::Feedback &feedback)
    {
        switch (drive_)
        {
        case Drive::scrub:
        {
            // A response curve gives fine control near the centre of the stick.
            const float push = std::copysign(std::pow(std::fabs(stick_), 1.6f), stick_);
            if (pushing_at_end())
            {
                // Rubber: past an end the stick no longer sets a speed, it
                // sets how far the row is stretched, and the row eases there.
                const float end = push < 0.0f ? 0.0f : last();
                const float stretch = end + (reduced() ? 0.0f : kRubber * push);
                wheel_.value += (stretch - wheel_.value) * (1.0f - std::exp(-kRubberRate * dt));
                wheel_.velocity = 0.0f;
            }
            else
            {
                // The wheel is heavy: it takes a moment to reach the stick's speed.
                wheel_.velocity += (push * kScrubSpeed - wheel_.velocity) *
                                   (1.0f - std::exp(-kScrubResponse * dt));
                wheel_.value += wheel_.velocity * dt;
            }
            break;
        }
        case Drive::coast:
            wheel_.velocity *= std::exp(-kFriction * dt);
            wheel_.value += wheel_.velocity * dt;
            if (beyond() != 0.0f)
            {
                // Ran into an end: the spring takes the remaining speed and
                // turns it into the bounce.
                wheel_.velocity = std::clamp(wheel_.velocity, -kMaxEndSpeed, kMaxEndSpeed);
                settle_on(nearest());
            }
            else if (std::fabs(wheel_.velocity) < kSnapSpeed)
            {
                catch_wheel();
            }
            break;
        case Drive::snap:
            wheel_.target = static_cast<float>(target_);
            wheel_.update(dt, reduced() ? 18.0f : kSnapOmega, reduced() ? 1.0f : kSnapDamping);
            break;
        }
        if (reduced())
            wheel_.value = std::clamp(wheel_.value, 0.0f, last());

        // Reaching an end by stick or by momentum is refused once per visit.
        // A press that was refused has had its sound already (quiet_), and the
        // last ripples of the spring around an end are not a new visit.
        const bool at_end = std::fabs(beyond()) > kEndSlack || pushing_at_end();
        if (at_end && !against_end_ && !quiet_)
        {
            const float side = wheel_.value < last() * 0.5f ? -1.0f : 1.0f;
            feedback.play(audio::Cue::error, 1.0f, 0.4f * side, 0.6f);
            feedback.rumble(0.25f, 0.05f);
        }
        against_end_ = at_end;

        const int centre = nearest();
        if (centre != centre_)
        {
            const float direction = centre > centre_ ? 1.0f : -1.0f;
            centre_ = centre;
            apply_palette(false);
            // Detents: one soft tick per cover that passes the centre, faster
            // spins a little higher, and never more than the ear can separate.
            if (!quiet_ && tick_wait_ <= 0.0f)
            {
                const float rate = tween::clamp01(std::fabs(wheel_.velocity) / kScrubSpeed);
                feedback.play(audio::Cue::tick, 0.94f + 0.22f * rate, 0.25f * direction, 0.8f);
                tick_wait_ = kTickInterval;
            }
        }
        else if (centre_ >= 0 && centre_ < count_)
        {
            // A cover arrived for the centred game: take its colours.
            const bool has = item(centre_).cover != 0;
            if (has != centre_had_cover_)
                apply_palette(false);
            centre_had_cover_ = has;
        }
    }

    // ---- geometry ----

    // Everything about one cover follows from d, its distance from the wheel
    // position: there is no "selected" state to switch, so scrubbing, coasting
    // and stepping all look right for free.
    Pose pose(int index) const
    {
        const float d = static_cast<float>(index) - wheel_.value;
        const float away = std::fabs(d);
        const float side = d < 0.0f ? -1.0f : 1.0f;
        const float soft = std::sqrt(d * d + kSoftAbs * kSoftAbs) - kSoftAbs;
        const float lift = tween::clamp01(lift_.value);

        float offset = side * (kSpreadA * (1.0f - std::exp(-away * kSpreadK)) + kSpreadB * away);
        float scale = kFarScale + (1.0f - kFarScale) * std::exp(-soft * kScaleFall);
        Pose result;
        result.light = kFarLight + (1.0f - kFarLight) * std::exp(-soft * kLightFall);
        result.alpha = tween::clamp01(kReach - away);

        // Entrance: the row closes in from both sides, centre first.
        const int rank = std::min(static_cast<int>(away + 0.5f), static_cast<int>(kReach));
        const float in = tween::stagger(age_, rank, 0.055f, 0.55f);
        result.alpha *= in;
        if (!reduced())
        {
            offset *= 1.0f + 0.3f * (1.0f - in);
            scale *= 0.92f + 0.08f * in;
        }
        // While a cover is open the others make room and fade.
        if (index != opened_)
        {
            offset += side * kOpenSlide * tween::cubic_in_out(lift);
            result.alpha *= 1.0f - lift;
        }

        // Bases rise toward the vanishing height as covers shrink, so the row
        // stands on a floor that recedes instead of hanging from a line.
        const float size = kCover * scale;
        const float base = kVanishY + (kBaseY - kVanishY) * scale;
        result.rect = {kCentreX + offset - size * 0.5f, base - size, size, size};

        if (index == opened_ && lift > 0.0f)
        {
            const float t = tween::cubic_in_out(lift);
            Rect open = kOpenRect;
            if (launch_.running && !reduced())
                open = open.inset(-14.0f * tween::ping(tween::cubic_out(launch_.progress())));
            result.rect = {
                tween::lerp(result.rect.x, open.x, t), tween::lerp(result.rect.y, open.y, t),
                tween::lerp(result.rect.w, open.w, t), tween::lerp(result.rect.h, open.h, t)};
            result.light = tween::lerp(result.light, 1.0f, t);
            // It left the floor: its mirror image goes first.
            result.reflect = 1.0f - tween::clamp01(lift * 3.0f);
        }
        return result;
    }

    // The picture's own shape inside the square a cover owns: as wide as the square and as high as the
    // picture is (or the other way round), standing on the square's bottom edge.
    static Rect fit_cover(const Rect &square, float aspect)
    {
        aspect = std::clamp(aspect, 0.55f, 1.8f);
        if (aspect >= 1.0f)
        {
            const float w = square.h / aspect;
            return {square.x + (square.w - w) * 0.5f, square.y, w, square.h};
        }
        const float h = square.w * aspect;
        return {square.x, square.y + square.h - h, square.w, h};
    }

    // The floor's colour at a height: the reflections are veiled with it.
    Color floor_at(float y) const
    {
        const float t = tween::clamp01((y - kFloorY) / (gfx::kVirtualHeight - kFloorY));
        return gfx::mix(palette_[3].value(), kNight, t);
    }

    // ---- drawing ----

    void draw_floor(gfx::DrawList &list) const
    {
        const Color top = floor_at(kFloorY);
        list.gradient_rect({0, kFloorY - kHorizon, gfx::kVirtualWidth, kHorizon}, 0,
                           top.with_alpha(0.0f), top);
        list.gradient_rect({0, kFloorY, gfx::kVirtualWidth, gfx::kVirtualHeight - kFloorY}, 0, top,
                           floor_at(gfx::kVirtualHeight));
    }

    void draw_header(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const cdi::Library &lib = cdi::library();
        const float in = tween::stagger(age_, 0, 0.05f, 0.5f);
        char text[96];
        if (lib.view.empty())
            std::snprintf(text, sizeof(text), "PHILIPS CD-i  \xC2\xB7  %d GAME%s", count_, count_ == 1 ? "" : "S");
        else
            std::snprintf(text, sizeof(text), "PHILIPS CD-i  \xC2\xB7  %s  \xC2\xB7  %d", lib.view.c_str(), count_);
        ui::text(list, fonts.semibold, text, kCentreX, kHeaderY - 10.0f * (1.0f - in), 18,
                 kWhite.with_alpha(0.6f * in), gfx::Align::center, 4.0f);
        // the Seedee5 logo, top left
        if (lib.logo)
            list.image(lib.logo, Rect{48.0f, 32.0f - 10.0f * (1.0f - in), 128.0f, 128.0f}, gfx::kFullUv,
                       {1.0f, 1.0f, 1.0f, in}, 24.0f);
        // What is still on its way: the NAS, then the covers, one small line each
        float y = kHeaderY + 34.0f;
        if (!lib.nas.empty())
        {
            const Color c = lib.nasState == 3 ? kWarn : lib.nasState == 2 ? kGood : kWhite.with_alpha(0.55f);
            ui::text(list, fonts.regular, fonts.regular.font->fit(lib.nas, 20, 1500), kCentreX, y, 20,
                     c.with_alpha(c.a * in), gfx::Align::center);
            y += 28.0f;
        }
        if (!lib.covers.empty())
            ui::text(list, fonts.regular, fonts.regular.font->fit(lib.covers, 20, 1500), kCentreX, y, 20,
                     kWhite.with_alpha(0.45f * in), gfx::Align::center);
    }

    void draw_empty(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 2, 0.08f, 0.5f);
        const cdi::Library &lib = cdi::library();
        list.push_opacity(in);
        if (!lib.view.empty())
        {
            // a view with nothing in it (no favourites yet...): say so, and how to get back to every game
            ui::text(list, fonts.display, "Nothing here yet", kCentreX, 470.0f, 64, kWhite, gfx::Align::center);
            ui::text(list, fonts.regular, fonts.regular.font->fit(lib.emptyText, 28, 1500), kCentreX, 540.0f, 28,
                     kWhite.with_alpha(0.7f), gfx::Align::center);
            const ui::Hint back[] = {{ui::Button::touchpad, "Show the next list"}, {ui::Button::options, "Settings"}};
            ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), back, 2, 1824, true);
            list.pop_opacity();
            return;
        }
        ui::text(list, fonts.display, "No games yet", kCentreX, 470.0f, 64, kWhite, gfx::Align::center);
        std::string where = "Put .chd, .cue, .iso or .7z files in " + lib.roomFor;
        ui::text(list, fonts.regular, fonts.regular.font->fit(where, 28, 1500), kCentreX, 540.0f, 28,
                 kWhite.with_alpha(0.7f), gfx::Align::center);
        ui::text(list, fonts.regular, "over FTP, or press OPTIONS to set up a NAS and the BIOS.",
                 kCentreX, 586.0f, 24, kWhite.with_alpha(0.5f), gfx::Align::center);
        const ui::Hint hints[] = {{ui::Button::options, "Settings"}, {ui::Button::square, "Look again"}};
        ui::draw_hints(list, fonts, ui::GlyphStyle::dark(), hints, 2, 1824, true);
        list.pop_opacity();
    }

    // A game with no picture yet: a dark card in its colours, with its title on it.
    void draw_card(gfx::DrawList &list, const cdi::Game &it, const Rect &r, float light, float radius) const
    {
        const Color top = gfx::mix(it.mid, kNight, 0.35f);
        const Color bottom = gfx::mix(it.dark, kNight, 0.5f);
        list.gradient_rect(r, radius, top.with_alpha(1.0f), bottom.with_alpha(1.0f));
        list.rounded_rect(r, radius, kBlack.with_alpha(1.0f - light));
        const ui::Fonts &fonts = context_.fonts;
        const float k = r.w / kCover;
        if (k < 0.3f)
            return;
        ui::text(list, fonts.semibold, "CD-i", r.x + 30.0f * k, r.y + 56.0f * k, 26.0f * k,
                 it.accent.with_alpha(0.8f * light), gfx::Align::left, 3.0f * k);
        ui::paragraph(list, fonts.display, it.title, r.x + 30.0f * k, r.y + r.h * 0.48f, 40.0f * k,
                      r.w - 60.0f * k, 48.0f * k, kWhite.with_alpha(0.9f * light), 3);
    }

    // One cover with its mirror image, shadow, glow and star. `focus` is how
    // much of the centre's light it gets.
    void draw_cover(gfx::DrawList &list, int index, const Pose &p, float focus) const
    {
        if (p.alpha <= 0.01f)
            return;
        const cdi::Game &it = item(index);
        const Rect square = p.rect;
        const Rect r = it.cover ? fit_cover(square, it.aspect) : square;
        const float k = square.w / kCover;
        const float radius = (it.cover ? 0.4f : 1.0f) * kRadius * k; // box art keeps its corners nearly square
        list.push_opacity(p.alpha);

        if (p.reflect > 0.01f && it.cover)
        {
            // The mirror image: the lower part of the artwork with its v range
            // running the other way (the texture's top row is v = 0, so
            // 1 -> 1 - part puts the cover's bottom edge at the top). It is drawn
            // opaque and dark so overlapping reflections hide each other like
            // the covers do, then veiled with the floor's own gradient until
            // nothing is left.
            const Rect mirror{r.x, r.y + r.h + 3.0f * k, r.w, r.h * kReflectPart * kReflectSquash};
            const float light = p.light * kReflectLight;
            list.image(it.cover, mirror, {0.0f, 1.0f, 1.0f, -kReflectPart}, {light, light, light, 1.0f}, radius);
            const Rect veil{mirror.x - 1.0f, mirror.y, mirror.w + 2.0f, mirror.h + 2.0f};
            const float haze = 1.0f - p.reflect * (1.0f - kReflectHaze);
            list.gradient_rect(veil, 0, floor_at(veil.y).with_alpha(haze), floor_at(veil.y + veil.h));
        }

        // The shadow separates overlapping covers and grounds each one.
        const float height = 1.0f - p.reflect; // 0 on the floor, 1 lifted
        list.shadow({r.x, r.y + (14.0f + 16.0f * height) * k, r.w, r.h}, radius,
                    (30.0f + 20.0f * height) * k, kBlack.with_alpha(0.55f));
        if (focus > 0.01f)
        {
            const float breath = reduced() ? 0.5f : ui::breathe(clock_);
            list.glow(r.inset(24.0f * k), radius, 80.0f, it.accent.with_alpha((0.24f + 0.1f * breath) * focus));
        }
        if (it.cover)
            list.image(it.cover, r, gfx::kFullUv, {p.light, p.light, p.light, 1.0f}, radius);
        else
            draw_card(list, it, r, p.light, radius);
        list.bordered_rect(r, radius, kClear, 1.5f, kWhite.with_alpha(0.16f * p.light));

        if (it.favorite)
        {
            // A badge on the corner; it pops when it is given.
            const bool fresh = index == (open_ ? opened_ : current());
            const float pop = fresh ? star_.value : 0.0f;
            const float cx = r.x + r.w - 40.0f * k;
            const float cy = r.y + 40.0f * k;
            list.circle(cx, cy, (24.0f + 6.0f * pop) * k, kInk.with_alpha(0.72f));
            list.star(cx, cy, (14.0f + 9.0f * pop) * k, gfx::mix(kInk, kGold, p.light));
        }
        list.pop_opacity();
    }

    void draw_row(gfx::DrawList &list, float lift) const
    {
        // Far covers first, so nearer ones overlap them.
        struct Entry
        {
            int index;
            float away;
        };
        Entry order[2 * static_cast<int>(kReach) + 3];
        int entries = 0;
        const int first = std::max(0, static_cast<int>(std::floor(wheel_.value - kReach)));
        const int end = std::min(count_ - 1, static_cast<int>(std::ceil(wheel_.value + kReach)));
        for (int i = first; i <= end && entries < static_cast<int>(std::size(order)); ++i)
            order[entries++] = {i, std::fabs(static_cast<float>(i) - wheel_.value)};
        std::sort(order, order + entries, [](const Entry &a, const Entry &b) { return a.away > b.away; });

        // The centre's light and ring wait, like the caption, for a slow wheel.
        const float focus = settled() * calm_.value * (1.0f - lift);
        for (int i = 0; i < entries; ++i)
        {
            const int index = order[i].index;
            if (index == opened_ && lift > 0.01f)
                continue; // drawn above the scrim
            draw_cover(list, index, pose(index), index == centre_ ? focus : 0.0f);
        }

        if (focus <= 0.01f)
            return;
        const Pose centre = pose(centre_);
        const cdi::Game &it = item(centre_);
        const Rect r = it.cover ? fit_cover(centre.rect, it.aspect) : centre.rect;
        const float in = tween::stagger(age_, 0, 0.055f, 0.55f);
        // The focus ring: it belongs to the centre of the row, and appears on
        // whichever cover comes to rest there.
        list.bordered_rect(r.inset(-7.0f), (it.cover ? 0.4f : 1.0f) * kRadius + 7.0f, kClear, 3.0f,
                           kWhite.with_alpha(0.95f * focus * in));
    }

    // Title and facts of the centred cover. There is one caption, always for
    // the nearest cover; it is opaque only while the wheel rests, so passing
    // titles fade through instead of flickering.
    void draw_caption(gfx::DrawList &list, float lift) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 5, 0.08f, 0.5f);
        const float shown = in * (1.0f - tween::clamp01(lift * 2.5f));
        // During a fast spin a counter takes the caption's place: digits in a
        // monospaced face can change every frame without flickering.
        const float blur = shown * (1.0f - calm_.value);
        if (blur > 0.01f)
        {
            char place[16];
            std::snprintf(place, sizeof(place), "%02d", centre_ + 1);
            ui::text(list, fonts.mono, place, kCentreX, kTitleY, 64, kWhite.with_alpha(0.5f * blur),
                     gfx::Align::center);
            std::snprintf(place, sizeof(place), "OF %d", count_);
            ui::text(list, fonts.semibold, place, kCentreX, kMetaY - 4.0f, 18, kWhite.with_alpha(0.4f * blur),
                     gfx::Align::center, 4.0f);
        }
        const float alpha = shown * settled() * calm_.value;
        if (alpha <= 0.01f)
            return;
        const cdi::Game &it = item(centre_);
        // The caption belongs to its cover: it stays under it, so a step
        // carries the old title out one side as the new one arrives from the
        // other, and a row stretched past its end takes its caption along.
        const float x = reduced() ? kCentreX : pose(centre_).rect.cx();
        const float rise = reduced() ? 0.0f : 14.0f * (1.0f - in);

        list.push_opacity(alpha);
        ui::text(list, fonts.display, fonts.display.font->fit(it.title, 64, 1200), x, kTitleY + rise, 64, kWhite,
                 gfx::Align::center);
        ui::text(list, fonts.regular, fonts.regular.font->fit(it.meta, 26, 1300), x, kMetaY + rise, 26,
                 kWhite.with_alpha(0.74f), gfx::Align::center);
        list.pop_opacity();
    }

    // Where the wheel is within the collection. The thumb is drawn from the
    // same position as the covers, so it shows the coasting and the stretch.
    void draw_scrub_bar(gfx::DrawList &list, float lift) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const float in = tween::stagger(age_, 7, 0.08f, 0.5f);
        list.push_opacity(in * (1.0f - tween::clamp01(lift * 2.5f)));
        list.rounded_rect({kBarX, kBarY - 2.0f, kBarW, 4.0f}, 2.0f, kWhite.with_alpha(0.14f));
        const float span = std::max(1.0f, last());
        if (count_ <= kMaxDots)
        {
            for (int i = 0; i < count_; ++i)
            {
                const float x = kBarX + kBarW * static_cast<float>(i) / span;
                if (item(i).favorite)
                    list.circle(x, kBarY, 4.5f, kGold);
                else
                    list.circle(x, kBarY, 2.5f, kWhite.with_alpha(0.36f));
            }
        }
        else
        {
            // A big list: a tick and its letter where each letter starts
            const cdi::Library &lib = cdi::library();
            float last_x = -100.0f;
            for (int b = 0; b < 27; ++b)
            {
                const int at = lib.letters[b];
                if (at < 0 || at >= count_ || bucket(item(at).letter) != b)
                    continue; // an empty letter repeats the next one
                const float x = kBarX + kBarW * static_cast<float>(at) / span;
                list.rounded_rect({x - 1.0f, kBarY - 7.0f, 2.0f, 14.0f}, 1.0f, kWhite.with_alpha(0.3f));
                if (x - last_x >= 22.0f)
                {
                    const char label[2] = {b == 0 ? '#' : static_cast<char>('A' + b - 1), 0};
                    ui::text(list, fonts.semibold, label, x, kBarY - 16.0f, 15, kWhite.with_alpha(0.45f),
                             gfx::Align::center);
                    last_x = x;
                }
            }
        }

        // Past an end the thumb is squeezed against it instead of leaving.
        const float over = beyond();
        const float squeeze = 1.0f - std::min(0.55f, std::fabs(over) * 1.5f);
        const float width = 44.0f * squeeze;
        const float at = kBarX + kBarW * tween::clamp01(wheel_.value / span);
        float left = at - width * 0.5f;
        if (over < 0.0f)
            left = at - 22.0f;
        else if (over > 0.0f)
            left = at + 22.0f - width;
        const Rect thumb{left, kBarY - 8.0f, width, 16.0f};
        const Color accent = palette_[4].value();
        list.glow(thumb, 8.0f, 16.0f, accent.with_alpha(0.45f));
        list.rounded_rect(thumb, 8.0f, gfx::mix(accent, kWhite, 0.35f));

        // The place in numbers; during a fast spin the large counter says it.
        char text[16];
        std::snprintf(text, sizeof(text), "%02d", centre_ + 1);
        ui::text(list, fonts.mono, text, kBarX - 30.0f, kBarY + 8.0f, 22, kWhite.with_alpha(0.9f * calm_.value),
                 gfx::Align::right);
        std::snprintf(text, sizeof(text), "%02d", count_);
        ui::text(list, fonts.mono, text, kBarX + kBarW + 30.0f, kBarY + 8.0f, 22,
                 kWhite.with_alpha(0.5f * calm_.value));
        list.pop_opacity();
    }

    // After L1 or R1: the letter the wheel landed on, large, fading out
    void draw_letter(gfx::DrawList &list, float lift) const
    {
        if (letter_shown_ <= 0.0f || lift > 0.5f)
            return;
        const float a = tween::clamp01(letter_shown_ / 0.4f);
        const char label[2] = {item(letter_index_).letter, 0};
        list.circle(kCentreX, 330.0f, 92.0f, kNight.with_alpha(0.6f * a));
        ui::text(list, context_.fonts.display, label, kCentreX, 368.0f, 110, kWhite.with_alpha(a), gfx::Align::center);
    }

    // The open game's screenshot over the whole screen (cut to fill it), dimmed, darker toward the panel and the
    // bottom so the cover and the text stay clear.
    void draw_backdrop(gfx::DrawList &list, float lift) const
    {
        const cdi::Game &it = item(opened_);
        const float a = tween::clamp01(backdrop_.value) * lift;
        if (!it.backdrop || a <= 0.01f)
            return;
        constexpr float W = gfx::kVirtualWidth, H = gfx::kVirtualHeight;
        const float aspect = it.backdropAspect > 0.05f ? it.backdropAspect : 0.75f; // height over width
        Rect uv = gfx::kFullUv;
        if (aspect > H / W) // taller than the screen: a band across its middle
        {
            const float part = (H / W) / aspect;
            uv = {0.0f, 0.5f - part * 0.5f, 1.0f, part};
        }
        else
        {
            const float part = aspect / (H / W);
            uv = {0.5f - part * 0.5f, 0.0f, part, 1.0f};
        }
        // a slow drift, so the still picture lives a little
        const float drift = reduced() ? 0.0f : 0.012f * std::sin(clock_ * 0.15f);
        uv.x += drift * uv.w;
        const float zoom = 1.04f;
        const Rect r{-W * (zoom - 1.0f) * 0.5f, -H * (zoom - 1.0f) * 0.5f, W * zoom, H * zoom};
        list.image(it.backdrop, r, uv, {0.62f, 0.62f, 0.66f, a});
        list.gradient_rect({0, 0, W, H}, 0, kNight.with_alpha(0.1f * a), kNight.with_alpha(0.6f * a));
        list.gradient_rect_h({W * 0.35f, 0, W * 0.65f, H}, 0, kNight.with_alpha(0.0f), kNight.with_alpha(0.45f * a));
    }

    void draw_details(gfx::DrawList &list, std::uint32_t glass, float lift) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const cdi::Game &it = item(opened_);
        char text[96];

        // The panel waits until the cover has moved out of its way, and on
        // the way back it is gone before the cover returns.
        const float appear_panel = tween::clamp01((lift - 0.45f) / 0.5f);
        Rect panel = kPanel;
        if (!reduced())
            panel.x += 80.0f * (1.0f - tween::cubic_out(appear_panel));
        list.push_opacity(appear_panel);
        list.shadow({panel.x, panel.y + 22, panel.w, panel.h}, kPanelRadius, 60, kBlack.with_alpha(0.5f));
        // Frosted panel: the blurred screen, a tint in the cover's own dark
        // tone, then a hairline of light.
        list.glass(glass, panel, kPanelRadius, kWhite);
        const Color tint = gfx::mix(gfx::mix(it.dark, it.mid, 0.18f), kInk, 0.35f);
        list.rounded_rect(panel, kPanelRadius, tint.with_alpha(0.64f));
        list.bordered_rect(panel, kPanelRadius, kClear, 1.5f, kWhite.with_alpha(0.2f));

        const float x = panel.x + kPanelPad;
        const float width = panel.w - 2.0f * kPanelPad;
        ui::text(list, fonts.semibold, "PHILIPS CD-i", x, panel.y + 84, 20, it.accent, gfx::Align::left, 4.0f);
        if (it.titleLogo && it.titleLogoAspect > 0.02f)
        {
            // the game's own logo in place of its name: at most 70 high and 460 wide, on the title's line
            float h = 70.0f, w = h / it.titleLogoAspect;
            if (w > std::min(460.0f, width))
            {
                w = std::min(460.0f, width);
                h = w * it.titleLogoAspect;
            }
            list.image(it.titleLogo, {x, panel.y + 166 - h, w, h}, gfx::kFullUv, kWhite);
        }
        else
            ui::text(list, fonts.display, fonts.display.font->fit(it.title, 60, width), x - 2, panel.y + 152, 60, kWhite);
        ui::paragraph(list, fonts.regular, it.meta, x, panel.y + 198, 26, width, 36, kWhite.with_alpha(0.72f), 2);
        // what the list of CD-i games knows about it (year, developer, publisher), and the time played with it
        std::string facts = it.year;
        for (const std::string *part : {&it.developer, &it.publisher})
            if (!part->empty() && (part != &it.publisher || it.publisher != it.developer))
                facts += (facts.empty() ? "" : "  \xC2\xB7  ") + *part;
        if (facts.empty())
            facts = it.name;
        ui::text(list, fonts.regular, fonts.regular.font->fit(facts, 22, width), x, panel.y + 270, 22, kWhite.with_alpha(0.6f));
        if (!it.played.empty())
            ui::text(list, fonts.regular, fonts.regular.font->fit(it.played, 20, width), x, panel.y + 302, 20,
                     it.accent.with_alpha(0.85f));

        // About the game (from its Wikipedia article), its review scores, and where the words are from. The space
        // is kept whether or not they come, so nothing moves when they arrive.
        {
            const float appear = tween::stagger(appear_panel, 0, 0.1f, 0.6f);
            list.push_opacity(appear);
            if (!it.about.empty())
                ui::paragraph(list, fonts.regular, it.about, x, panel.y + 352, 22, width, 32, kWhite.with_alpha(0.86f), 5);
            else if (it.aboutState == 0)
                ui::text(list, fonts.regular, "Looking this game up \xE2\x80\xA6", x, panel.y + 352, 22,
                         kWhite.with_alpha(0.35f + 0.15f * ui::breathe(clock_)));
            if (!it.reviews.empty())
            {
                list.star(x + 10, panel.y + 504, 10.0f, kGold, 0.0f);
                ui::text(list, fonts.semibold, fonts.semibold.font->fit(it.reviews, 22, width - 34), x + 30,
                         panel.y + 512, 22, kWhite.with_alpha(0.9f));
            }
            if (!it.credit.empty())
                ui::text(list, fonts.regular, fonts.regular.font->fit(it.credit, 16, width), x, panel.y + 548, 16,
                         kWhite.with_alpha(0.4f));
            list.pop_opacity();
        }

        // Four facts, arriving one after the other once the panel is there.
        constexpr float kTileGap = 16.0f;
        const float tile_w = (width - 3.0f * kTileGap) / 4.0f;
        const char *labels[] = {"WHERE", "DISCS", "FORMAT", "READY"};
        for (int i = 0; i < 4; ++i)
        {
            const float appear = tween::stagger(appear_panel, i, 0.1f, 0.6f);
            const Rect tile{x + static_cast<float>(i) * (tile_w + kTileGap),
                            panel.y + 584 + (reduced() ? 0.0f : 22.0f * (1.0f - appear)), tile_w, 100};
            list.push_opacity(appear);
            list.rounded_rect(tile, 22, kWhite.with_alpha(0.08f));
            ui::text(list, fonts.semibold, labels[i], tile.x + 22, tile.y + 34, 15, kWhite.with_alpha(0.55f),
                     gfx::Align::left, 3.0f);
            Color value = kWhite;
            switch (i)
            {
            case 0:
                std::snprintf(text, sizeof(text), "%s", it.source.c_str());
                break;
            case 1:
                std::snprintf(text, sizeof(text), "%d", it.discs);
                break;
            case 2:
                std::snprintf(text, sizeof(text), "%s", it.format.c_str());
                break;
            default:
                std::snprintf(text, sizeof(text), "%s", it.here ? "Yes" : "Copy first");
                value = it.here ? kGood : kWarn;
                break;
            }
            ui::text(list, fonts.semibold, fonts.semibold.font->fit(text, 30, tile.w - 40), tile.x + 22,
                     tile.y + 78, 30, value);
            list.pop_opacity();
        }

        // The one focused thing on the panel: the Play button, lit in the
        // cover's accent. It swells when pressed.
        const float press = launch_.running ? tween::ping(launch_.progress()) : 0.0f;
        const float breath = reduced() ? 0.5f : ui::breathe(clock_);
        const Rect play = Rect{x, panel.y + 718, 290, 68}.inset(reduced() ? 0.0f : -4.0f * press);
        list.glow(play, 34, 22, it.accent.with_alpha(0.3f + 0.2f * breath + 0.4f * press));
        list.rounded_rect(play, 34, kWhite);
        ui::draw_button(list, fonts, ui::GlyphStyle::light(), ui::Button::cross, play.x + 24, play.cy(), 34);
        const bool resume = !it.resume.empty();
        const char *verb = launch_.running || cdi::playing() ? "Starting \xE2\x80\xA6"
                           : resume ? "Continue"
                                    : (it.here ? "Play" : "Copy and play");
        ui::text(list, fonts.semibold, verb, play.x + 74, play.cy() + 10, 28, kInk);
        float after = x + 290 + 28;
        if (resume)
        {
            // the automatic save's time under it, and Square to start from the beginning beside it
            ui::text(list, fonts.regular, "where you left it, " + it.resume, x + 8, panel.y + 818, 20,
                     kWhite.with_alpha(0.55f));
            const Rect again{after, panel.y + 722, 220, 60};
            list.rounded_rect(again, 30, kWhite.with_alpha(0.12f));
            ui::draw_button(list, fonts, ui::GlyphStyle::dark(), ui::Button::square, again.x + 18, again.cy(), 30);
            ui::text(list, fonts.semibold, "Start over", again.x + 62, again.cy() + 9, 24, kWhite);
            after += 220 + 24;
        }

        // Beside them, the favourite state: not focusable, Triangle toggles it.
        const bool starred = it.favorite;
        const float chip_x = after;
        const float chip_cy = panel.y + 752;
        list.star(chip_x + 16, chip_cy, 15.0f + 8.0f * star_.value, starred ? kGold : kWhite.with_alpha(0.7f),
                  starred ? 0.0f : 2.5f);
        ui::text(list, fonts.regular, starred ? "In your favourites" : "Not a favourite yet", chip_x + 44,
                 chip_cy + 9, 24, kWhite.with_alpha(starred ? 0.9f : 0.6f));
        list.pop_opacity();
    }

    void draw_hints(gfx::DrawList &list, float lift) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const ui::GlyphStyle style = ui::GlyphStyle::dark();
        if (open_)
        {
            const bool resume = opened_ >= 0 && opened_ < count_ && !item(opened_).resume.empty();
            const ui::Hint hints[] = {{ui::Button::cross, resume ? "Continue" : "Play"},
                                      {ui::Button::square, "Start over"},
                                      {ui::Button::triangle, "Favourite"},
                                      {ui::Button::circle, "Back"}};
            const ui::Hint plain[] = {{ui::Button::cross, "Play"},
                                      {ui::Button::triangle, "Favourite"},
                                      {ui::Button::circle, "Back"}};
            list.push_opacity(tween::clamp01(lift * 2.0f));
            if (resume)
                ui::draw_hints(list, fonts, style, hints, 4, 1824, true);
            else
                ui::draw_hints(list, fonts, style, plain, 3, 1824, true);
            list.pop_opacity();
        }
        else
        {
            const ui::Hint hints[] = {{ui::Button::left_stick, "Spin"},
                                      {ui::Button::l1, "Letter", ui::Button::r1},
                                      {ui::Button::cross, "Open"},
                                      {ui::Button::triangle, "Favourite"},
                                      {ui::Button::touchpad, "Lists"},
                                      {ui::Button::options, "Settings"}};
            list.push_opacity(tween::stagger(age_, 9, 0.08f, 0.5f) * (1.0f - lift));
            ui::draw_hints(list, fonts, style, hints, 6, 1824, true);
            list.pop_opacity();
        }
    }

    app::Context &context_;
    int count_ = 0;
    float age_ = 0.0f;   // seconds since enter(): drives the entrance
    float clock_ = 0.0f; // free-running time for idle motion

    // The wheel. value is the position in covers (3.5 is half-way between the
    // fourth and fifth), velocity is covers per second. In Drive::snap it is
    // a spring toward target_; otherwise update() moves it directly.
    tween::Bounce wheel_;
    Drive drive_ = Drive::snap;
    int target_ = 0;
    float stick_ = 0.0f;       // the stick's deflection while scrubbing
    int centre_ = 0;           // the cover nearest the position, as last announced
    bool centre_had_cover_ = false;
    bool quiet_ = true;        // the current movement already had its sound
    bool against_end_ = false; // the wheel is past an end (refuse once)
    float tick_wait_ = 0.0f;   // seconds until the next tick may play
    float letter_shown_ = 0.0f; // seconds the big letter still shows
    float letter_held_[2] = {0.0f, 0.0f}; // how long L1 / R1 have been held
    float dt_ = 0.0f;
    int letter_index_ = 0;

    tween::Spring calm_; // 1 while the wheel is slow enough to read a caption
    ui::SpringColor palette_[5];
    ui::Pulse star_;

    bool open_ = false;
    bool was_playing_ = false;
    int opened_ = 0;     // the cover the details belong to
    tween::Spring lift_; // 0 in the row, 1 lifted beside the panel
    tween::Spring backdrop_; // the open game's screenshot: 0 none, 1 shown
    tween::Timer launch_;
};

} // namespace

std::unique_ptr<app::Concept> make_cdiwheel(app::Context &context)
{
    return std::make_unique<CoverFlow>(context);
}

} // namespace hui::screen

// The title's screen, for its program (ps5/ui/kit.hpp declares it)
namespace ps5ui
{
std::unique_ptr<hui::app::Concept> make_screen(hui::app::Context &context)
{
    return hui::screen::make_cdiwheel(context);
}
} // namespace ps5ui
