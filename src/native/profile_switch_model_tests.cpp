#include "profile_switch_model.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
constexpr std::size_t camera_size = 0x600;
constexpr float native_fov = 0.523598776F;
constexpr float third_fov = 0.785398163F;
void require(const bool value, const char* message) { if (!value) throw std::runtime_error(message); }

class FakeAccess final : public gdtpc::CameraMemoryAccess
{
public:
    std::array<std::byte, camera_size> bytes{};
    int fail_write{-1};
    int fail_read_field{-1};
    int writes{};
    [[nodiscard]] bool preflight(const void*, const std::size_t offset, bool) noexcept override
    {
        return offset + sizeof(float) <= bytes.size();
    }
    [[nodiscard]] bool read(const void*, const std::size_t offset, void* destination, const std::size_t size) noexcept override
    {
        if (offset + size > bytes.size()) return false;
        for (std::size_t index=0; index<gdtpc::camera_field_count; ++index)
            if (gdtpc::camera_field_offsets[index]==offset && static_cast<int>(index)==fail_read_field) return false;
        std::memcpy(destination, bytes.data() + offset, size); return true;
    }
    [[nodiscard]] bool write(void*, const std::size_t offset, const void* source, const std::size_t size) noexcept override
    {
        const auto operation = writes++;
        if (operation == fail_write) return false;
        if (offset + size > bytes.size()) return false;
        std::memcpy(bytes.data() + offset, source, size); return true;
    }
};

gdtpc::CameraRawSnapshot snapshot(const gdtpc::CameraProfile& profile, const float zoom,
    const float fov = native_fov)
{
    gdtpc::CameraRawSnapshot value{};
    require(gdtpc::make_profile_snapshot(profile, zoom, fov, value), "snapshot creation failed"); return value;
}
void seed(FakeAccess& access, const gdtpc::CameraRawSnapshot& value)
{
    access.bytes.fill(std::byte{0x5a});
    for (std::size_t index=0; index<gdtpc::camera_field_count; ++index)
        std::memcpy(access.bytes.data()+gdtpc::camera_field_offsets[index],value.fields[index].data(),sizeof(float));
}
void write_snapshot(FakeAccess& access, const gdtpc::CameraRawSnapshot& value)
{
    for (std::size_t index=0; index<gdtpc::camera_field_count; ++index)
        std::memcpy(access.bytes.data()+gdtpc::camera_field_offsets[index],value.fields[index].data(),sizeof(float));
}
gdtpc::CameraSessionIdentity identity(FakeAccess& access, const std::uint64_t generation=1, const std::uintptr_t player=3)
{
    return {reinterpret_cast<std::uintptr_t>(access.bytes.data()),2,player,generation};
}
gdtpc::ProfileFrame frame(FakeAccess& access, const bool key=false, const bool foreground=true,
    const std::uint64_t generation=1, const std::uintptr_t player=3, const bool stop=false)
{
    return {identity(access,generation,player),access.bytes.data(),foreground,key,stop};
}
}

int main()
{
    try
    {
        const gdtpc::CameraProfile native_profile{{20,48,36},{38,52,46}};
        const gdtpc::CameraProfile third_profile{{12,90,42},{20,44,30}};
        const auto native=snapshot(native_profile,36);

        // Release-arm, one transition per physical press, exact return, and transition-only writes.
        {
            FakeAccess access; seed(access,native); const auto original=access.bytes;
            gdtpc::ProfileSwitchModel model(third_profile, third_fov);
            require(model.step(access,frame(access)).transition==gdtpc::ProfileTransition::none,"release did not arm cleanly");
            const auto entered=model.step(access,frame(access,true));
            require(entered.transition==gdtpc::ProfileTransition::entered_third_person&&entered.mode==gdtpc::ProfileMode::third_person,
                "F8 edge did not enter third person");
            gdtpc::CameraRawSnapshot entered_snapshot{};
            require(gdtpc::capture_camera_snapshot(access,access.bytes.data(),entered_snapshot)&&
                std::bit_cast<float>(entered_snapshot.fields[gdtpc::camera_fov_field_index])==third_fov,
                "ThirdPerson entry did not apply the configured FOV exactly");
            const auto writes_after_entry=access.writes;
            require(model.step(access,frame(access,true)).transition==gdtpc::ProfileTransition::none&&access.writes==writes_after_entry,
                "held F8 repeated a transition");
            static_cast<void>(model.step(access,frame(access,false)));
            const auto returned=model.step(access,frame(access,true));
            require(returned.transition==gdtpc::ProfileTransition::returned_native&&returned.mode==gdtpc::ProfileMode::native,
                "second F8 edge did not return native");
            require(access.bytes==original&&!returned.dirty,"native snapshot was not restored byte-for-byte");
        }

        // No player and background input cannot enter; focus loss restores and requires release to rearm.
        {
            FakeAccess access; seed(access,native); const auto original=access.bytes;
            gdtpc::ProfileSwitchModel model(third_profile, third_fov);
            static_cast<void>(model.step(access,frame(access,false,true,1,0)));
            require(model.step(access,frame(access,true,true,1,0)).transition==gdtpc::ProfileTransition::none&&access.writes==0,
                "no-player frame wrote camera memory");
            static_cast<void>(model.step(access,frame(access,false)));
            require(model.step(access,frame(access,true)).transition==gdtpc::ProfileTransition::entered_third_person,
                "eligible edge did not enter");
            // Alt-tab retains the chosen mode by request: nothing is written, nothing is restored,
            // and the key must still be disarmed so no toggle can be taken in the background.
            const auto entered_bytes=access.bytes;
            const auto writes_before_background=access.writes;
            const auto background=model.step(access,frame(access,true,false));
            require(background.transition==gdtpc::ProfileTransition::none&&
                background.mode==gdtpc::ProfileMode::third_person&&background.dirty&&
                access.bytes==entered_bytes&&access.writes==writes_before_background,
                "focus loss did not retain third person without writing");
            require(model.step(access,frame(access,true,true)).transition==gdtpc::ProfileTransition::none&&
                access.bytes==entered_bytes,
                "held key replayed after focus regain");
            // A release and a fresh press still returns to Native exactly.
            static_cast<void>(model.step(access,frame(access,false,true)));
            const auto returned=model.step(access,frame(access,true,true));
            require(returned.transition==gdtpc::ProfileTransition::returned_native&&access.bytes==original,
                "toggle after focus regain did not restore native bytes exactly");
        }

        // Stop in third person restores on the callback and disables new control.
        {
            FakeAccess access; seed(access,native); const auto original=access.bytes;
            gdtpc::ProfileSwitchModel model(third_profile, third_fov); static_cast<void>(model.step(access,frame(access)));
            static_cast<void>(model.step(access,frame(access,true)));
            const auto stopped=model.step(access,frame(access,true,true,1,3,true));
            require(stopped.transition==gdtpc::ProfileTransition::returned_native&&stopped.restore_state==gdtpc::ProfileRestoreState::verified&&
                !stopped.control_enabled&&!stopped.dirty&&access.bytes==original,"stop did not restore and disable control");
        }

        // A clean session replacement rebinds and clears remembered zoom; a dirty replacement
        // never writes an old snapshot to the new session.
        {
            FakeAccess access; seed(access,native); gdtpc::ProfileSwitchModel model(third_profile, third_fov);
            static_cast<void>(model.step(access,frame(access)));
            const auto clean_rebind=model.step(access,frame(access,false,true,2));
            require(clean_rebind.mode==gdtpc::ProfileMode::native&&!clean_rebind.dirty,"clean session did not rebind native");
        }

        // Independent zoom: sample the outgoing current blend on the toggle frame, restore an
        // arbitrary native snapshot bit-for-bit, then apply the remembered third-person distance
        // exactly once on the next entry. Native changes between excursions remain untouched.
        {
            FakeAccess access;
            auto arbitrary_native=snapshot({{7,83,31},{11,61,37}},26);
            arbitrary_native.fields[7]=std::bit_cast<std::array<std::byte,sizeof(float)>>(0.8F);
            seed(access,arbitrary_native); const auto first_native_bytes=access.bytes;
            gdtpc::ProfileSwitchModel model(third_profile, third_fov);
            static_cast<void>(model.step(access,frame(access)));
            require(model.step(access,frame(access,true)).transition==gdtpc::ProfileTransition::entered_third_person,
                "first Gate 2 entry failed");
            static_cast<void>(model.step(access,frame(access,false)));
            const auto scrolled=snapshot(third_profile,82.2F,third_fov); // blend 0.9
            write_snapshot(access,scrolled); // native zoom processing between transitions
            const auto writes_before_return=access.writes;
            const auto returned=model.step(access,frame(access,true));
            require(returned.transition==gdtpc::ProfileTransition::returned_native&&access.bytes==first_native_bytes,
                "Gate 2 return did not restore arbitrary native bytes exactly");
            require(access.writes>writes_before_return,"Gate 2 return performed no transition write");

            auto later_native=snapshot({{5,65,25},{9,55,33}},29);
            later_native.fields[7]=std::bit_cast<std::array<std::byte,sizeof(float)>>(0.7F);
            write_snapshot(access,later_native); const auto later_native_bytes=access.bytes;
            const auto writes_during_native=access.writes;
            static_cast<void>(model.step(access,frame(access,false)));
            require(access.writes==writes_during_native&&access.bytes==later_native_bytes,
                "native zoom processing was overwritten between transitions");
            require(model.step(access,frame(access,true)).transition==gdtpc::ProfileTransition::entered_third_person,
                "remembered Gate 2 entry failed");
            gdtpc::CameraRawSnapshot remembered{};
            require(gdtpc::capture_camera_snapshot(access,access.bytes.data(),remembered),"remembered profile capture failed");
            require(std::abs(gdtpc::zoom_distance_from_snapshot(remembered)-82.2F)<0.0001F,
                "outgoing third-person zoom was not remembered from the toggle frame");
            static_cast<void>(model.step(access,frame(access,false)));
            require(model.step(access,frame(access,true)).transition==gdtpc::ProfileTransition::returned_native&&
                access.bytes==later_native_bytes,"latest native excursion snapshot was not restored exactly");
        }

        // Both third-person endpoints are remembered, and a generation change resets that memory
        // so the next session starts from the configured default instead of stale zoom.
        for (const auto endpoint : {12.0F,90.0F})
        {
            FakeAccess access; seed(access,native); gdtpc::ProfileSwitchModel model(third_profile, third_fov);
            static_cast<void>(model.step(access,frame(access))); static_cast<void>(model.step(access,frame(access,true)));
            static_cast<void>(model.step(access,frame(access,false)));
            write_snapshot(access,snapshot(third_profile,endpoint,third_fov));
            static_cast<void>(model.step(access,frame(access,true)));
            static_cast<void>(model.step(access,frame(access,false)));
            static_cast<void>(model.step(access,frame(access,true)));
            gdtpc::CameraRawSnapshot remembered{};
            require(gdtpc::capture_camera_snapshot(access,access.bytes.data(),remembered)&&
                std::abs(gdtpc::zoom_distance_from_snapshot(remembered)-endpoint)<0.0001F,
                "third-person endpoint zoom was not remembered");
            static_cast<void>(model.step(access,frame(access,false)));
            static_cast<void>(model.step(access,frame(access,true)));
            write_snapshot(access,native);
            static_cast<void>(model.step(access,frame(access,false,true,2)));
            require(model.step(access,frame(access,true,true,2)).transition==gdtpc::ProfileTransition::entered_third_person,
                "new session did not accept an entry");
            gdtpc::CameraRawSnapshot reset{};
            require(gdtpc::capture_camera_snapshot(access,access.bytes.data(),reset)&&
                std::abs(gdtpc::zoom_distance_from_snapshot(reset)-42.0F)<0.0001F,
                "session change retained stale third-person zoom");
        }
        {
            FakeAccess access; seed(access,native); gdtpc::ProfileSwitchModel model(third_profile, third_fov);
            static_cast<void>(model.step(access,frame(access))); static_cast<void>(model.step(access,frame(access,true)));
            const auto writes=access.writes;
            const auto changed=model.step(access,frame(access,false,true,2));
            require(changed.restore_state==gdtpc::ProfileRestoreState::impossible&&changed.transition==gdtpc::ProfileTransition::rejected_fault&&
                access.writes==writes,"dirty session replacement attempted a stale restore");
            require(model.abandoned_excursion_count()==0,"a still-resident profile was counted as abandoned");
            // A live object really is still modified, so the toggle must stay latched off.
            static_cast<void>(model.step(access,frame(access,false,true,2)));
            require(model.step(access,frame(access,true,true,2)).transition==gdtpc::ProfileTransition::rejected_fault&&
                access.writes==writes,"a still-modified camera re-armed the toggle");
        }

        // Live-observed case: the session changes and the game reinitializes the camera itself.
        // Nothing of ours survives, so the excursion is abandoned and counted. The user's mode
        // intent persists, so the next stable frame re-enters automatically and still restores the
        // new session's own native bytes exactly when F8 is pressed.
        {
            FakeAccess access; seed(access,native); gdtpc::ProfileSwitchModel model(third_profile, third_fov);
            static_cast<void>(model.step(access,frame(access)));
            static_cast<void>(model.step(access,frame(access,true)));
            require(model.dirty(),"setup did not enter third person");
            seed(access,native); // the game reloads the world and restores its own native profile
            const auto reseeded=access.bytes;
            const auto writes=access.writes;
            const auto abandoned=model.step(access,frame(access,false,true,2));
            require(abandoned.transition==gdtpc::ProfileTransition::returned_native&&!abandoned.dirty&&
                abandoned.mode==gdtpc::ProfileMode::native&&
                abandoned.restore_state==gdtpc::ProfileRestoreState::abandoned&&
                access.writes==writes&&access.bytes==reseeded,
                "an abandoned excursion wrote memory or misreported its state");
            require(model.abandoned_excursion_count()==1,"the abandoned excursion was not counted");
            const auto re_entered=model.step(access,frame(access,false,true,2));
            require(re_entered.transition==gdtpc::ProfileTransition::entered_third_person&&
                re_entered.restore_state==gdtpc::ProfileRestoreState::clean,
                "third person did not re-enter after an abandoned excursion");
            const auto back=model.step(access,frame(access,true,true,2));
            require(back.transition==gdtpc::ProfileTransition::returned_native&&access.bytes==reseeded,
                "the recovered session did not restore its own native bytes exactly");
            require(model.abandoned_excursion_count()==1,"a clean return was counted as abandoned");
        }

        // A rift/load can temporarily make the player identity invalid before the camera is reset.
        // Keep the live third-person excursion without reading an invalid player or writing anything;
        // when the same logical identity returns, mouse look can recapture without an F8 press.
        {
            FakeAccess access; seed(access,native); gdtpc::ProfileSwitchModel model(third_profile, third_fov);
            static_cast<void>(model.step(access,frame(access))); static_cast<void>(model.step(access,frame(access,true)));
            const auto resident=access.bytes; const auto writes=access.writes;
            const auto loading=model.step(access,frame(access,false,true,2,0));
            require(loading.transition==gdtpc::ProfileTransition::none&&loading.dirty&&
                loading.mode==gdtpc::ProfileMode::third_person&&loading.control_enabled&&
                access.bytes==resident&&access.writes==writes,
                "transient player loss dropped or faulted the third-person excursion");
            const auto resumed=model.step(access,frame(access,false,true,1,3));
            require(resumed.transition==gdtpc::ProfileTransition::none&&resumed.dirty&&
                resumed.mode==gdtpc::ProfileMode::third_person&&access.bytes==resident&&access.writes==writes,
                "the same session did not resume third person after a transient load");
        }

        // If the game resets the profile during a player-less rift/world load, the confirmed camera
        // proves every invariant native byte is back. Abandon safely, retain user intent, and apply a
        // fresh profile from the next valid generation's own exact native preimage.
        {
            FakeAccess access; seed(access,native); gdtpc::ProfileSwitchModel model(third_profile, third_fov);
            static_cast<void>(model.step(access,frame(access))); static_cast<void>(model.step(access,frame(access,true)));
            seed(access,native); const auto game_reinitialized=access.bytes; const auto writes=access.writes;
            const auto loading=model.step(access,frame(access,false,true,2,0));
            require(loading.transition==gdtpc::ProfileTransition::returned_native&&
                loading.restore_state==gdtpc::ProfileRestoreState::abandoned&&!loading.dirty&&
                access.bytes==game_reinitialized&&access.writes==writes&&model.abandoned_excursion_count()==1,
                "the player-less load did not safely abandon the game-reset profile");
            const auto reentered=model.step(access,frame(access,false,true,3,4));
            require(reentered.transition==gdtpc::ProfileTransition::entered_third_person&&reentered.dirty&&
                reentered.mode==gdtpc::ProfileMode::third_person,
                "third-person intent did not survive a rift/world load");
            static_cast<void>(model.step(access,frame(access,false,true,3,4)));
            const auto returned=model.step(access,frame(access,true,true,3,4));
            require(returned.transition==gdtpc::ProfileTransition::returned_native&&access.bytes==game_reinitialized,
                "the auto-reentered generation did not restore its own native bytes exactly");
        }

        // A stop during the player-less part of a load cancels pending re-entry. A later valid
        // generation stays native and receives no writes.
        {
            FakeAccess access; seed(access,native); gdtpc::ProfileSwitchModel model(third_profile, third_fov);
            static_cast<void>(model.step(access,frame(access))); static_cast<void>(model.step(access,frame(access,true)));
            seed(access,native);
            const auto stopped=model.step(access,frame(access,false,true,2,0,true));
            require(!stopped.control_enabled&&!stopped.dirty&&stopped.mode==gdtpc::ProfileMode::native,
                "stop during a world load did not cancel the abandoned excursion");
            const auto writes=access.writes;
            const auto next=model.step(access,frame(access,false,true,3,4));
            require(next.mode==gdtpc::ProfileMode::native&&!next.dirty&&access.writes==writes,
                "a stopped runtime re-entered third person after the load");
        }

        // Scrolling changes only current/target blend; the invariant third-person profile remains
        // resident and must never be mistaken for game reinitialization on a session change.
        {
            FakeAccess access; seed(access,native); gdtpc::ProfileSwitchModel model(third_profile, third_fov);
            static_cast<void>(model.step(access,frame(access))); static_cast<void>(model.step(access,frame(access,true)));
            write_snapshot(access,snapshot(third_profile,75,third_fov));
            const auto writes=access.writes;
            const auto changed=model.step(access,frame(access,false,true,2));
            require(changed.transition==gdtpc::ProfileTransition::rejected_fault&&
                changed.restore_state==gdtpc::ProfileRestoreState::impossible&&access.writes==writes&&
                model.abandoned_excursion_count()==0,
                "native scrolling made a resident third-person profile look abandoned");
        }

        // Read failure during the residence check is indeterminate and therefore latches safely;
        // it is never treated as proof that the game removed our profile.
        {
            FakeAccess access; seed(access,native); gdtpc::ProfileSwitchModel model(third_profile, third_fov);
            static_cast<void>(model.step(access,frame(access))); static_cast<void>(model.step(access,frame(access,true)));
            access.fail_read_field=2; const auto writes=access.writes;
            const auto changed=model.step(access,frame(access,false,true,2));
            require(changed.transition==gdtpc::ProfileTransition::rejected_fault&&
                changed.restore_state==gdtpc::ProfileRestoreState::impossible&&access.writes==writes&&
                model.abandoned_excursion_count()==0,
                "indeterminate profile residence was incorrectly abandoned");
        }

        // A partial game reset that changes only FOV is not proof that every runtime-owned field
        // disappeared. It must latch instead of abandoning a still-modified camera.
        {
            FakeAccess access; seed(access,native); gdtpc::ProfileSwitchModel model(third_profile, third_fov);
            static_cast<void>(model.step(access,frame(access))); static_cast<void>(model.step(access,frame(access,true)));
            std::memcpy(access.bytes.data()+gdtpc::camera_field_offsets[gdtpc::camera_fov_field_index],
                native.fields[gdtpc::camera_fov_field_index].data(),sizeof(float));
            const auto writes=access.writes;
            const auto changed=model.step(access,frame(access,false,true,2));
            require(changed.transition==gdtpc::ProfileTransition::rejected_fault&&
                changed.restore_state==gdtpc::ProfileRestoreState::impossible&&access.writes==writes&&
                model.abandoned_excursion_count()==0,
                "a partial FOV reset was incorrectly treated as complete game restoration");
        }

        // A nonfinite current blend rejects a return without writing or sanitizing suspicious
        // state; control latches and the dirty excursion remains truthful.
        {
            FakeAccess access; seed(access,native); gdtpc::ProfileSwitchModel model(third_profile, third_fov);
            static_cast<void>(model.step(access,frame(access))); static_cast<void>(model.step(access,frame(access,true)));
            static_cast<void>(model.step(access,frame(access,false)));
            const auto nan=std::bit_cast<std::array<std::byte,sizeof(float)>>(std::numeric_limits<float>::quiet_NaN());
            std::memcpy(access.bytes.data()+0x580,nan.data(),nan.size()); const auto writes=access.writes;
            const auto rejected=model.step(access,frame(access,true));
            require(rejected.transition==gdtpc::ProfileTransition::rejected_fault&&rejected.dirty&&
                rejected.restore_state==gdtpc::ProfileRestoreState::pending&&access.writes==writes,
                "nonfinite outgoing zoom was sanitized or written");
        }

        // A recoverable entry fault restores the native preimage exactly and latches control off.
        {
            FakeAccess access; seed(access,native); const auto original=access.bytes; access.fail_write=4;
            gdtpc::ProfileSwitchModel model(third_profile, third_fov); static_cast<void>(model.step(access,frame(access)));
            const auto failed=model.step(access,frame(access,true));
            require(failed.transition==gdtpc::ProfileTransition::rejected_fault&&!failed.control_enabled&&!failed.dirty&&
                failed.restore_state==gdtpc::ProfileRestoreState::verified&&access.bytes==original,
                "recoverable entry fault did not leave verified native state");
            require(failed.control_write_count==4&&failed.restore_write_count==11,"fault write accounting was incorrect");
        }

        // The controlled fault must restore the latest native scroll state, including a distinct
        // in-progress current/target pair, rather than an older/default native zoom.
        {
            FakeAccess access;
            auto scrolling_native=snapshot({{3,73,28},{8,58,35}},51);
            scrolling_native.fields[7]=std::bit_cast<std::array<std::byte,sizeof(float)>>(0.95F);
            seed(access,scrolling_native); const auto exact_scrolling_bytes=access.bytes;
            access.fail_write=4;
            gdtpc::ProfileSwitchModel model(third_profile, third_fov); static_cast<void>(model.step(access,frame(access)));
            const auto failed=model.step(access,frame(access,true));
            require(failed.transition==gdtpc::ProfileTransition::rejected_fault&&!failed.dirty&&
                failed.restore_state==gdtpc::ProfileRestoreState::verified&&access.bytes==exact_scrolling_bytes,
                "recoverable fault did not restore the latest native scroll snapshot exactly");
        }

        std::cout<<"PASS: Gate 2 independent zoom and FOV, arbitrary and endpoint snapshots, transition-only writes, exact "
                   "native return, session reset, conservative residence checks, alt-tab retention, stop restore, "
                   "abandoned-excursion reporting, and fault accounting.\n";
        return 0;
    }
    catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
