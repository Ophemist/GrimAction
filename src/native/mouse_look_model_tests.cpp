#include "mouse_look_model.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
void require(const bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void near(const float actual, const float expected, const char* message, const float tolerance = 0.0001F)
{
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) throw std::runtime_error(message);
}

gdtpc::MouseLookSettings enabled_settings()
{
    gdtpc::MouseLookSettings s{};
    s.enabled = true;
    // Aim-band tests below predate vertical look; a zero pitch range sends all vertical movement to the aim.
    s.pitch_offset_min = 0.0F;
    s.pitch_offset_max = 0.0F;
    return s;
}

// A 1000 x 800 client area at screen (100, 50): centre x = 600, aim 0.5 -> y = 450.
gdtpc::MouseLookInput frame(const std::int32_t x, const std::int32_t y)
{
    gdtpc::MouseLookInput in{};
    in.eligible = true;
    in.mouse_input_active = true;
    in.cursor_valid = true;
    in.cursor_x = x;
    in.cursor_y = y;
    in.client = {100, 50, 1100, 850};
    return in;
}
}

int main()
{
    try
    {
        // Settings validation.
        require(gdtpc::valid_mouse_look_settings(enabled_settings()), "default settings rejected");
        auto bad = enabled_settings(); bad.yaw_radians_per_pixel = 0.0F;
        require(!gdtpc::valid_mouse_look_settings(bad), "zero sensitivity accepted");
        bad = enabled_settings(); bad.yaw_radians_per_pixel = std::numeric_limits<float>::quiet_NaN();
        require(!gdtpc::valid_mouse_look_settings(bad), "NaN sensitivity accepted");
        bad = enabled_settings(); bad.aim_band_top = 0.8F;
        require(!gdtpc::valid_mouse_look_settings(bad), "inverted aim band accepted");
        bad = enabled_settings(); bad.aim_start = 0.9F;
        require(!gdtpc::valid_mouse_look_settings(bad), "aim start outside the band accepted");
        bad = enabled_settings(); bad.menu_release_frames = 0;
        require(!gdtpc::valid_mouse_look_settings(bad), "zero menu debounce accepted");
        {
            gdtpc::MouseLookModel model(bad);
            require(model.step(frame(600, 450)).state == gdtpc::MouseLookState::disabled && !model.captured(),
                "invalid settings captured the cursor");
        }
        {
            gdtpc::MouseLookModel model(gdtpc::MouseLookSettings{});
            require(model.step(frame(600, 450)).state == gdtpc::MouseLookState::disabled,
                "a disabled configuration captured the cursor");
        }

        // Capture edge re-anchors: warps to the aim point, clips, and never rotates.
        {
            gdtpc::MouseLookModel model(enabled_settings());
            auto d = model.step(frame(123, 777));
            require(d.state == gdtpc::MouseLookState::captured && d.capture_edge && d.reanchor && d.warp,
                "the first eligible frame did not capture with a re-anchor");
            require(d.warp_x == 600 && d.warp_y == 450, "the aim point is not client centre at aim_start");
            near(d.yaw_delta, 0.0F, "a capture edge rotated the camera");

            // Horizontal movement becomes yaw; vertical slides the aim point.
            d = model.step(frame(700, 450));
            require(!d.capture_edge && !d.reanchor, "a steady captured frame re-anchored");
            near(d.yaw_delta, 100 * 0.003F, "yaw delta is not pixels times sensitivity");
            require(d.warp_x == 600 && d.warp_y == 450, "the cursor was not returned to the aim point");
            d = model.step(frame(600, 530));
            near(d.yaw_delta, 0.0F, "vertical movement rotated the camera");
            near(d.aim_y, 0.6F, "80 px down did not move the aim 0.1 of an 800 px client");
            require(d.warp_y == 530, "the warp did not follow the new aim point");

            // The band clamps in both directions.
            d = model.step(frame(600, 5000));
            near(d.aim_y, 0.75F, "the aim point passed the bottom of the band");
            require(d.warp_y == 650, "the clamped warp is wrong");
            d = model.step(frame(600, -5000));
            near(d.aim_y, 0.25F, "the aim point passed the top of the band");

            // Left Alt is a hard override: released, clip restored, no rotation, even in combat.
            auto alt = frame(900, 250);
            alt.alt_down = true;
            alt.combat = true;
            alt.menu_raw = true;
            d = model.step(alt);
            require(d.state == gdtpc::MouseLookState::alt && !d.captured && d.release_edge && !d.warp,
                "Left Alt did not release the cursor");
            near(d.yaw_delta, 0.0F, "a released frame rotated the camera");
            d = model.step(alt);
            require(!d.release_edge, "release edge repeated while Alt stayed held");
            alt.menu_raw = false; // the menu closes while Alt is still held; let its debounce settle
            for (int i = 0; i < 3; ++i) static_cast<void>(model.step(alt));

            // Returning from Alt re-anchors: the cursor displacement made while free is discarded.
            d = model.step(frame(1000, 800));
            require(d.capture_edge && d.reanchor, "Alt release did not re-anchor");
            near(d.yaw_delta, 0.0F, "movement made while Alt was held spun the camera");
            near(d.aim_y, 0.25F, "the aim point did not persist across the release");
        }

        // Menus release the cursor unless in combat; the release is debounced.
        {
            gdtpc::MouseLookModel model(enabled_settings());
            static_cast<void>(model.step(frame(600, 450)));
            auto menu = frame(600, 450);
            menu.menu_raw = true;
            auto d = model.step(menu);
            require(d.state == gdtpc::MouseLookState::menu && d.release_edge, "a menu did not release the cursor at once");
            d = model.step(frame(600, 450));
            require(d.state == gdtpc::MouseLookState::menu, "one clear frame ended the menu (single-frame gap)");
            d = model.step(frame(600, 450));
            require(d.state == gdtpc::MouseLookState::menu, "two clear frames ended the menu");
            d = model.step(frame(600, 450));
            require(d.state == gdtpc::MouseLookState::captured && d.reanchor, "three clear frames did not recapture with a re-anchor");

            menu.combat = true;
            d = model.step(menu);
            require(d.state == gdtpc::MouseLookState::captured && !d.release_edge,
                "a menu opened in combat released the cursor");
            menu.alt_down = true;
            d = model.step(menu);
            require(d.state == gdtpc::MouseLookState::alt, "Left Alt did not override combat");

            // A menu that closes while Alt is held is already settled when Alt is released.
            auto held = frame(600, 450); held.alt_down = true;
            for (int i = 0; i < 3; ++i) static_cast<void>(model.step(held));
            d = model.step(frame(600, 450));
            require(d.state == gdtpc::MouseLookState::captured, "debounce did not advance while Alt was held");
        }

        // UI panel bytes: ordinary panels are independent, while Escape needs its companion byte.
        // A lone primary Escape byte is the live rift/Alt-click regression and must not hold mouse look.
        {
            require(gdtpc::classify_panel_open_flags(0, 0, 0, 0, 0, 0, 0, 0) == 0, "closed UI reported a panel");
            require(gdtpc::classify_panel_open_flags(1, 0, 0, 0, 0, 0, 0, 0) == 0x01, "inventory flag was missed");
            require(gdtpc::classify_panel_open_flags(0, 1, 0, 0, 0, 0, 0, 0) == 0x02, "quest flag was missed");
            require(gdtpc::classify_panel_open_flags(0, 0, 1, 0, 0, 0, 0, 0) == 0x04, "skills flag was missed");
            require(gdtpc::classify_panel_open_flags(0, 0, 0, 1, 0, 0, 0, 0) == 0x08, "map flag was missed");
            require(gdtpc::classify_panel_open_flags(0, 0, 0, 0, 1, 0, 0, 0) == 0,
                "a lone rift/Alt-click latch was mistaken for Escape");
            require(gdtpc::classify_panel_open_flags(0, 0, 0, 0, 0, 1, 0, 0) == 0,
                "a lone Escape companion byte reported a panel");
            require(gdtpc::classify_panel_open_flags(0, 0, 0, 0, 1, 1, 0, 0) == 0x10,
                "paired Escape flags were missed");
            require(gdtpc::classify_panel_open_flags(0, 0, 0, 0, 0, 0, 1, 0) == 0x20,
                "Factions flag was missed");
            require(gdtpc::classify_panel_open_flags(0, 0, 0, 0, 0, 0, 0, 1) == 0x40,
                "Loot Filter flag was missed");
            require(gdtpc::classify_panel_open_flags(2, 0x7f, 0xff, 2, 1, 1, 2, 0xff) == 0x10,
                "non-flag padding values reported ordinary panels");
            require(gdtpc::classify_panel_open_flags(1, 1, 1, 1, 1, 1, 1, 1) == 0x7f,
                "combined panel flags were classified incorrectly");
        }

        // Ineligibility wins over everything, and bad windows or cursor reads never produce rotation.
        {
            gdtpc::MouseLookModel model(enabled_settings());
            static_cast<void>(model.step(frame(600, 450)));
            auto off = frame(900, 450);
            off.eligible = false;
            auto d = model.step(off);
            require(d.state == gdtpc::MouseLookState::ineligible && d.release_edge && !d.warp, "ineligibility did not release");

            static_cast<void>(model.step(frame(600, 450)));
            auto tiny = frame(600, 450);
            tiny.client = {0, 0, 10, 10};
            d = model.step(tiny);
            require(d.state == gdtpc::MouseLookState::ineligible && !d.warp, "a minimised client area captured the cursor");

            static_cast<void>(model.step(frame(600, 450)));
            static_cast<void>(model.step(frame(600, 450)));
            auto unread = frame(9999, 9999);
            unread.cursor_valid = false;
            d = model.step(unread);
            require(d.captured && d.reanchor, "a failed cursor read did not re-anchor");
            near(d.yaw_delta, 0.0F, "a failed cursor read rotated the camera");

            auto moved = frame(900, 450);
            moved.client = {200, 50, 1200, 850};
            d = model.step(moved);
            require(d.reanchor && d.warp_x == 700, "a moved window did not re-anchor to its new centre");
            near(d.yaw_delta, 0.0F, "a moved window rotated the camera");
        }

        // Controller mode releases the OS cursor. Returning to the game's mouse mode captures through
        // a re-anchor, discarding any cursor displacement made while the controller was active.
        {
            gdtpc::MouseLookModel model(enabled_settings());
            static_cast<void>(model.step(frame(600, 450)));
            static_cast<void>(model.step(frame(700, 450)));
            auto controller = frame(975, 725);
            controller.mouse_input_active = false;
            auto d = model.step(controller);
            require(d.state == gdtpc::MouseLookState::ineligible && d.release_edge && !d.captured && !d.warp,
                "controller mode did not release mouse capture");
            d = model.step(controller);
            require(!d.release_edge && !d.warp, "controller mode repeated its release edge or warped the cursor");
            d = model.step(frame(975, 725));
            require(d.state == gdtpc::MouseLookState::captured && d.capture_edge && d.reanchor && d.warp,
                "returning to mouse mode did not recapture through a re-anchor");
            near(d.yaw_delta, 0.0F, "cursor movement made in controller mode rotated the camera on return");
        }

        // Inversion flips only the horizontal sign; reset forgets capture and restores aim_start.
        {
            auto inverted = enabled_settings();
            inverted.invert_x = true;
            gdtpc::MouseLookModel model(inverted);
            static_cast<void>(model.step(frame(600, 450)));
            auto d = model.step(frame(650, 450));
            near(d.yaw_delta, -50 * 0.003F, "invert_x did not flip the yaw sign");
            static_cast<void>(model.step(frame(600, 600)));
            model.reset();
            require(!model.captured(), "reset kept the capture");
            near(model.aim_y(), 0.5F, "reset did not restore aim_start");
            d = model.step(frame(700, 450));
            require(d.capture_edge && d.reanchor, "the first frame after reset rotated instead of re-anchoring");
        }

        // Vertical look: pitch first, then spill into the aim band, retracing the same path on reversal.
        {
            auto pitch_settings = enabled_settings();
            pitch_settings.pitch_offset_min = -15.0F;
            pitch_settings.pitch_offset_max = 25.0F;
            gdtpc::MouseLookModel model(pitch_settings);
            auto d = model.step(frame(600, 450));
            require(d.apply_pitch, "a captured frame did not apply pitch");
            near(d.pitch_offset_degrees, 0.0F, "pitch offset did not start at zero");
            d = model.step(frame(600, 550));
            near(d.pitch_offset_degrees, 10.0F, "100 px down did not add 10 degrees of pitch");
            near(d.aim_y, 0.5F, "the aim point moved before the pitch limit");
            require(d.warp_y == 450, "pitch-only movement moved the warp point");
            d = model.step(frame(600, 650));
            near(d.pitch_offset_degrees, 25.0F, "the pitch offset passed its maximum");
            near(d.aim_y, 0.5625F, "movement past the pitch limit did not spill 50 px into the aim band");
            d = model.step(frame(600, d.warp_y - 100));
            near(d.aim_y, 0.5F, "moving up did not return the spilled aim point first");
            near(d.pitch_offset_degrees, 20.0F, "the remainder did not reduce the pitch offset");
            d = model.step(frame(600, d.warp_y - 400));
            near(d.pitch_offset_degrees, -15.0F, "the pitch offset passed its minimum");
            near(d.aim_y, 0.4375F, "movement past the minimum did not spill upward");

            auto alt = frame(600, 100); alt.alt_down = true;
            d = model.step(alt);
            require(d.apply_pitch, "Alt stopped applying the pitch offset (camera would snap)");
            near(d.pitch_offset_degrees, -15.0F, "Alt changed the pitch offset");
            auto menu = frame(600, 100); menu.menu_raw = true;
            d = model.step(menu);
            require(d.apply_pitch && d.state == gdtpc::MouseLookState::menu, "a menu stopped applying the pitch offset");
            auto off = frame(600, 100); off.eligible = false;
            d = model.step(off);
            require(!d.apply_pitch, "an ineligible frame applied the pitch offset");
            model.reset();
            near(model.pitch_offset(), 0.0F, "reset kept the pitch offset");

            gdtpc::MouseLookModel disabled(gdtpc::MouseLookSettings{});
            require(!disabled.step(frame(600, 450)).apply_pitch, "a disabled configuration applied pitch");

            auto bad_pitch = pitch_settings; bad_pitch.pitch_offset_min = 5.0F;
            require(!gdtpc::valid_mouse_look_settings(bad_pitch), "a positive pitch minimum was accepted");
            bad_pitch = pitch_settings; bad_pitch.yaw_catchup_per_second = -1.0F;
            require(!gdtpc::valid_mouse_look_settings(bad_pitch), "a negative catch-up rate was accepted");
        }

        // With the native pitch known, the offset stops where the final pitch meets the floor and further upward
        // movement spills into the aim band; coming back down responds immediately (no dead zone).
        {
            auto floored = enabled_settings();
            floored.pitch_offset_min = -60.0F;
            floored.pitch_offset_max = 25.0F;
            floored.pitch_floor_degrees = -5.0F;
            floored.pitch_degrees_per_pixel = 0.05F; // the staged sensitivity these pixel counts assume
            gdtpc::MouseLookModel model(floored);
            auto in = frame(600, 450); in.native_pitch_valid = true; in.native_pitch_degrees = 17.0F;
            static_cast<void>(model.step(in));
            in.cursor_y = 450 - 1000; // 1000 px up: 440 px reach the -22 offset floor, 560 px spill into the aim band
            auto d = model.step(in);
            near(d.pitch_offset_degrees, -22.0F, "the offset did not stop at the pitch floor", 0.01F);
            near(d.aim_y, 0.25F, "movement past the floor did not spill into the aim band");
            auto down = frame(600, d.warp_y + 20); down.native_pitch_valid = true; down.native_pitch_degrees = 17.0F;
            d = model.step(down);
            require(d.aim_y > 0.25F, "the first downward movement did not act on the spilled aim point");
            // Unknown native pitch falls back to the configured minimum.
            gdtpc::MouseLookModel unknown(floored);
            auto u = frame(600, 450); static_cast<void>(unknown.step(u));
            u.cursor_y = 450 - 1000;
            d = unknown.step(u);
            near(d.pitch_offset_degrees, -50.0F, "without a native pitch the configured minimum did not apply", 0.01F);
        }

        // Catch-up fraction and pitch application helpers.
        near(gdtpc::yaw_catchup_fraction(6.0F, 0.016F), 1.0F - std::exp(-0.096F), "catch-up fraction is not exponential");
        near(gdtpc::yaw_catchup_fraction(0.0F, 0.016F), 0.0F, "a zero catch-up rate moved the camera");
        near(gdtpc::yaw_catchup_fraction(6.0F, 0.0F), 0.0F, "a zero frame time moved the camera");
        near(gdtpc::yaw_catchup_fraction(6.0F, 0.5F), 0.0F, "a stalled frame jumped the camera");
        near(gdtpc::apply_pitch_offset(0.3F, 5.0F, 1.0F), 0.3F + 5.0F * 0.0174532925F, "pitch offset was not added");
        near(gdtpc::apply_pitch_offset(0.3F, -40.0F, 1.0F), 0.0174532925F, "pitch fell below the floor");
        near(gdtpc::apply_pitch_offset(0.3F, -40.0F, -45.0F), (17.188734F - 40.0F) * 0.0174532925F,
            "a negative floor did not allow looking above horizontal", 0.001F);
        near(gdtpc::apply_pitch_offset(0.3F, -200.0F, -45.0F), -45.0F * 0.0174532925F, "pitch passed a negative floor");

        // Pitch overlay ownership survives the engine's degree round trip without compounding.
        {
            gdtpc::PitchOverlay overlay;
            const auto native = 0.3F;
            auto written = overlay.step(native, 10.0F, -45.0F);
            near(written, 0.3F + 10.0F * 0.0174532925F, "first overlay write is wrong");
            for (int frame = 0; frame < 200; ++frame)
            {
                // Engine clamp: radians -> degrees -> radians, as UpdateFromInputImpl does every frame.
                const auto roundtrip = (written * 57.2957763671875F) * 0.01745329238474369F;
                written = overlay.step(roundtrip, 10.0F, -45.0F);
            }
            near(written, 0.3F + 10.0F * 0.0174532925F, "the overlay compounded through the engine round trip", 0.0002F);
            near(overlay.native(), native, "the saved native pitch drifted", 0.0002F);
            // Below horizontal the engine clamps our value to exactly 0 each frame; that must stay ours.
            {
                gdtpc::PitchOverlay below;
                auto value = below.step(0.3F, -30.0F, -45.0F);
                require(value < 0.0F, "a -30 degree offset on 17 degrees did not go below horizontal");
                for (int frame = 0; frame < 50; ++frame)
                {
                    const auto clamped = value < 0.0F ? 0.0F : value; // UpdateFromInputImpl's [0, 89] clamp
                    value = below.step(clamped, -30.0F, -45.0F);
                    near(below.native(), 0.3F, "the engine's clamp to 0 was mistaken for a new native pitch");
                }
                near(value, 0.3F - 30.0F * 0.0174532925F, "the below-horizontal pitch flickered", 0.0002F);
            }
            // A genuinely new native pitch (zoom changed) is adopted.
            written = overlay.step(0.5F, 10.0F, -45.0F);
            near(overlay.native(), 0.5F, "a fresh native pitch was not adopted");
            near(written, 0.5F + 10.0F * 0.0174532925F, "the overlay did not follow the new native pitch");
            // Changing the offset on our own value recomposes from native rather than stacking.
            written = overlay.step(written, 20.0F, -45.0F);
            near(written, 0.5F + 20.0F * 0.0174532925F, "an offset change stacked on the previous overlay");
            overlay.release();
            written = overlay.step(0.4F, 0.0F, -45.0F);
            near(overlay.native(), 0.4F, "release did not forget ownership");
            require(std::isnan(overlay.step(std::numeric_limits<float>::quiet_NaN(), 5.0F, -45.0F)),
                "a nonfinite pitch field was not passed through untouched");
        }
        near(gdtpc::apply_pitch_offset(1.5F, 30.0F, 1.0F), 89.0F * 0.0174532925F, "pitch passed the engine's 89 degree clamp");
        near(gdtpc::apply_pitch_offset(0.3F, std::numeric_limits<float>::quiet_NaN(), 1.0F), 0.3F, "a NaN offset changed pitch");

        // NPC conversation classification: only a readable ControllerPlayer whose top state is TalkToNpc is talking.
        {
            constexpr std::uintptr_t player_vtable = 0x1000, talk_vtable = 0x2000, other_state = 0x3000;
            using gdtpc::NpcTalkSignal;
            gdtpc::PlayerControllerSample s{true, player_vtable, 1, talk_vtable};
            require(gdtpc::classify_npc_talk(s, player_vtable, talk_vtable) == NpcTalkSignal::talking, "TalkToNpc not detected");
            s.top_state_vtable = other_state;
            require(gdtpc::classify_npc_talk(s, player_vtable, talk_vtable) == NpcTalkSignal::idle, "another state counted as talking");
            // No temporary states: the executing state is the regular current state, which is the common case.
            s = {true, player_vtable, 0, talk_vtable};
            require(gdtpc::classify_npc_talk(s, player_vtable, talk_vtable) == NpcTalkSignal::talking,
                "the regular current state was ignored when no temporary states exist");
            s = {true, player_vtable, 0, 0};
            require(gdtpc::classify_npc_talk(s, player_vtable, talk_vtable) == NpcTalkSignal::idle, "a missing state counted as talking");
            s = {true, 0x4444, 1, talk_vtable};
            require(gdtpc::classify_npc_talk(s, player_vtable, talk_vtable) == NpcTalkSignal::not_player_controller,
                "a foreign object was trusted as the player controller");
            s = {false, player_vtable, 1, talk_vtable};
            require(gdtpc::classify_npc_talk(s, player_vtable, talk_vtable) == NpcTalkSignal::unknown, "an unreadable sample was trusted");
            s = {true, player_vtable, 1, talk_vtable};
            require(gdtpc::classify_npc_talk(s, 0, talk_vtable) == NpcTalkSignal::unknown &&
                gdtpc::classify_npc_talk(s, player_vtable, 0) == NpcTalkSignal::unknown, "unresolved vtables were trusted");
        }

        // Dot cursor handle ownership.
        {
            constexpr std::uintptr_t game_arrow = 0x111, game_attack = 0x222, dot = 0x999;
            gdtpc::CursorHandleOverlay cursor;
            std::uintptr_t value = 0, restore = 0;
            require(!cursor.step(game_arrow, 0, value) && !cursor.owned(), "a null dot handle was stored");
            require(cursor.step(game_arrow, dot, value) && value == dot && cursor.base() == game_arrow, "the dot did not replace the game cursor");
            require(cursor.step(dot, dot, value) && cursor.base() == game_arrow, "the dot was mistaken for the game cursor");
            require(cursor.step(game_attack, dot, value) && value == dot && cursor.base() == game_attack,
                "a game cursor change while owned was not adopted as the handle to restore");
            require(cursor.relinquish(dot, restore) && restore == game_attack && !cursor.owned(), "release did not restore the game cursor");
            require(cursor.step(game_arrow, dot, value), "step after release refused");
            require(!cursor.relinquish(game_attack, restore), "release overwrote a cursor the game set after the dot");
            // Ownership lost with the dot still stored: re-acquiring must not adopt the dot as the game cursor.
            require(cursor.step(game_arrow, dot, value), "step refused");
            cursor.forget();
            require(cursor.step(dot, dot, value) && cursor.base() == game_arrow, "a stale dot was adopted as the game cursor");
            require(cursor.relinquish(dot, restore) && restore == game_arrow, "the stale dot was not replaced by the game cursor");
        }

        // Right-stick pitch: deadzone, scaling and sign, speed and frame-time guards, and the model applying it within limits.
        {
            near(gdtpc::stick_pitch_degrees(8000, 90.0F, false, 0.1F), 0.0F, "inside the deadzone moved pitch");
            near(gdtpc::stick_pitch_degrees(32767, 90.0F, false, 0.1F), -9.0F, "full stick up does not look up at the configured speed");
            near(gdtpc::stick_pitch_degrees(-32768, 90.0F, false, 0.1F), 9.0F, "full stick down does not look down");
            near(gdtpc::stick_pitch_degrees(32767, 90.0F, true, 0.1F), 9.0F, "invert did not flip the direction");
            const auto half = gdtpc::stick_pitch_degrees(static_cast<std::int16_t>(8689 + (32767 - 8689) / 2), 90.0F, false, 0.1F);
            near(half, -4.5F, "half past the deadzone is not half speed", 0.01F);
            near(gdtpc::stick_pitch_degrees(32767, 90.0F, false, 0.5F), 0.0F, "an implausible frame time moved pitch");
            near(gdtpc::stick_pitch_degrees(32767, 0.0F, false, 0.1F), 0.0F, "a zero speed moved pitch");
            near(gdtpc::steam_stick_pitch_degrees(-6.0F, 90.0F, false, 0.1F), -9.0F,
                "Steam action 36 up does not look up at the configured speed");
            near(gdtpc::steam_stick_pitch_degrees(6.0F, 90.0F, false, 0.1F), 9.0F,
                "Steam action 36 down does not look down");
            near(gdtpc::steam_stick_pitch_degrees(-3.0F, 90.0F, false, 0.1F), -4.5F,
                "half Steam deflection is not half speed");
            near(gdtpc::steam_stick_pitch_degrees(-20.0F, 90.0F, true, 0.1F), 9.0F,
                "Steam action clamp/invert failed");
            near(gdtpc::steam_stick_pitch_degrees(std::numeric_limits<float>::quiet_NaN(), 90.0F, false, 0.1F), 0.0F,
                "nonfinite Steam action moved pitch");

            auto s = enabled_settings();
            s.pitch_offset_min = -20.0F;
            s.pitch_offset_max = 25.0F;
            s.pitch_floor_degrees = -6.0F;
            gdtpc::MouseLookModel model(s);
            auto in = frame(600, 450);
            in.native_pitch_valid = true;
            in.native_pitch_degrees = 10.0F;
            in.pitch_stick_degrees = 3.0F;
            auto d = model.step(in); // capture edge: re-anchor, the stick still applies
            require(d.reanchor, "the first frame was not a re-anchor");
            near(d.pitch_offset_degrees, 3.0F, "the stick did not apply on a re-anchor frame");
            in.pitch_stick_degrees = 100.0F;
            near(model.step(in).pitch_offset_degrees, 25.0F, "the stick exceeded the pitch offset maximum");
            in.pitch_stick_degrees = -100.0F;
            near(model.step(in).pitch_offset_degrees, -16.0F, "the stick passed the pitch floor (-6 - native 10)");
            near(model.step(in).aim_y, 0.5F, "the stick spilled into the aim band");
            in.alt_down = true;
            in.pitch_stick_degrees = 10.0F;
            near(model.step(in).pitch_offset_degrees, -16.0F, "the stick moved pitch while Alt freed the cursor");

            gdtpc::MouseLookModel controller_model(s);
            auto controller = frame(600, 450);
            controller.mouse_input_active = false;
            controller.controller_input_active = true;
            controller.native_pitch_valid = true;
            controller.native_pitch_degrees = 10.0F;
            controller.pitch_stick_degrees = -3.0F;
            d = controller_model.step(controller);
            require(d.state == gdtpc::MouseLookState::controller && !d.captured && !d.warp && d.apply_pitch,
                "controller pitch captured or warped the cursor, or failed to hold the overlay");
            near(d.pitch_offset_degrees, -3.0F, "controller-only state did not apply stick pitch");
            controller.menu_raw = true;
            controller.pitch_stick_degrees = -3.0F;
            d = controller_model.step(controller);
            require(d.state == gdtpc::MouseLookState::menu && d.apply_pitch,
                "controller menu did not suspend input while holding pitch");
            near(d.pitch_offset_degrees, -3.0F, "controller pitch moved while a menu was open");
        }

        std::cout << "PASS: panel flag classification with paired Escape confirmation and lone Alt-click rejection, right-stick pitch, dot cursor handle ownership, NPC conversation classification, vertical look pitch-then-aim spill with reversal, pitch held through Alt and menus, catch-up and pitch clamps, mouse-look settings validation, disabled and invalid configurations, capture-edge "
                     "re-anchor, pixel-to-yaw conversion, aim-band slide and clamping, Left Alt hard override in "
                     "combat and menus, re-anchor after every release, debounced menu release with the single-frame "
                     "gap, combat holding capture through a menu, debounce progress while Alt is held, "
                     "ineligibility, controller-mode release and zero-delta mouse-mode recapture, minimised windows, "
                     "failed cursor reads, moved windows, inversion and reset.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
