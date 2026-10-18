# VAmbience – GTA IV Complete Edition (1.2.0.59) plugin-sdk port

Port of ClonkAndre's **VAmbience** (https://github.com/ClonkAndre/VAmbience) from its private
ScriptHook-style SDK to a native **plugin-sdk** `.asi` for GTA IV Complete Edition **1.2.0.59**.
Original mod © ClonkAndre (see `LICENSE.original`); sound assets are his.

## Build (needs Windows + Visual Studio 2022, 32-bit)
1. Set up plugin-sdk as usual (`plugin_sdk` env var, build the `Plugin_IV` static lib – Release, Win32).
2. Create a new GTA IV plugin with the SDK's project generator ("Generate MyPlugin.bat" /
   the VS extension), name it `VAmbience`.
3. Replace the generated `Main.cpp` with `VAmbience.cpp` from this folder.
4. Project settings (the generator sets them for Plugin_IV): C++17, Win32,
   defines `GTAIV; PLUGIN_SGV_CE; RAGE`, link `Plugin_IV.lib`.
5. Build Release|Win32. Rename the output (`VAmbience.IV.asi`) to `VAmbience.asi`.

## Install
Copy into the GTA IV folder (next to GTAIV.exe), with an ASI loader installed:
```
VAmbience.asi   VAmbience.ini   bass.dll   VAmbiance\   (from ../package)
```
`bass.dll` is the 32-bit BASS 2.4 DLL from the original release; it is loaded at run time, so no
BASS headers/libs are needed to build.

## What changed vs. the original
Needed for the port
* ScriptHook-style `scriptTick` → `Events::gameProcessEvent`; natives via the SDK's `Command<>`.
* SFX volume read from the game's menu prefs (`aPrefs[PREF_SFX_VOLUME]`) instead of a fixed settings address.
* `INI.h` → Win32 profile API; static BASS link → run-time `LoadLibrary`.
* The hook also fires on the main menu, so the mod waits until a player exists.

Bug fixes made while porting (behaviour you may notice)
* Unknown zone name no longer dereferences `end()` (crash) – it just skips.
* The gun-burst worker thread (which called script natives off-thread) is now a timer on the game thread.
* Random picks are inclusive – the original `RANDOM_INT(1,N)` never picked the last group/file.
* Sound files are counted once at start-up instead of scanning disk on every sound.
* Leaked `BASS_3DVECTOR` allocations removed; paths are Unicode-safe.
* 3D audio: listener "front" is now the camera direction (the original passed Euler angles), and
  GTA axes are mapped to BASS axes. "Around the player" offsets are ±distance (original: only +x/+y).
* Pause menu now actually pauses/resumes running sounds.
* `GlobalMultiplier` is accepted in both spellings (the shipped ini says `GlobalMuliplier`).

## Verified against your GTAIV.exe (1.2.0.59, File version 1.2.0.59, unpacked)
* `Events::gameProcessEvent` pattern → 0x5C2B7A (unique)
* `rage::scr_resolver` full SDK pattern → 0x86FCE0 (unique); `s_CurrentThread` → 0x86D060
* `aPrefs` → 0x1160C48; entries 23 and 25 (music / SFX) both default to 9
* all 37 native hashes used by the mod are present in the executable
* source passes a clang syntax check against the SDK's real `Commands` enum

## NOT verified
* **The .asi has not been compiled or run** – the build sandbox has no Windows toolchain.
* The Aiden secret creates a ped from model hash 215190023 (same as the original); this is a custom
  model, so it only works if that model is installed. Test it first, or set `Secrets Enabled=0`.
* SDK native calls run on a dummy script thread; natives that attach to the calling script
  (`CREATE_CHAR`) are the most likely to misbehave.
