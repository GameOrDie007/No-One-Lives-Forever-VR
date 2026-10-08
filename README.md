# No One Lives Forever VR

No One Lives Forever (2000) in room-scale VR: native stereo, head tracking
and motion controls, through OpenXR.

This is a mod. It ships no game content: you supply your own copy of NOLF.

**Watch the video:** [No One Lives Forever in VR (it's a swinging 60s spy game)](https://youtu.be/IrHUTnWPAw8)

---

## Install

1. Unzip this release into a folder of its own, on your Desktop or in a games
   folder: anywhere except `Program Files` (setup needs to write there).

2. Double-click `Setup.bat`.

   It finds your NOLF and copies it into the `game` folder beside it. It looks
   in the usual places on every drive. If it cannot find it, drag your game
   folder onto `Setup.bat`, or drag both GOTY disc images (`.iso`) onto it at
   once, and it builds the game from the discs. A `.zip` of an installed folder
   works too.

   **Your original copy is never written to.** Setup makes a copy, so you can
   keep playing the flat game exactly as before.

   On a copy that has never been run, setup starts the original game once, in a
   small window, to build its font cache. The window closes on its own.

3. Connect your headset: start streaming in Virtual Desktop, or start SteamVR
   or Meta Quest Link. With a SteamVR headset (a Steam Frame, an Index), start
   SteamVR first and then the game, and the game uses SteamVR by itself.

4. Double-click `Play No One Lives Forever VR.bat`, then put the headset on.

If the headset is not connected yet, a notice says the game is waiting for it
and closes by itself when it connects. After a minute with no headset, the game
closes and says what to do.

The desktop shows a steady view from your right eye while you play, for
streaming or recording: it follows your turns but not the small movements of
your head, and keeps the horizon level (VR Options > Screen and Aim > Steady desktop
view turns it off for the plain eye). Alt-tab freely: the game keeps running
in the headset.

**What you need:** *No One Lives Forever*, Game of the Year edition, patched to
1.004 (the release with the four bonus missions); a PC VR headset with an
OpenXR runtime (developed on a Quest 3 over
[Virtual Desktop](https://www.vrdesktop.net/), and played on a Steam Frame over
SteamVR; Meta Quest Link provides OpenXR too, untested); and Windows 10 or 11,
64-bit.

**Updating to a new version:** unzip the new version into its own folder and
drag your previous version's `game` folder onto its `Setup.bat`. Your saves and
settings come with it; the new VR files are kept.

If something goes wrong, double-click `Collect report.bat`. It saves a zip
of the latest logs on your Desktop to send with your report.

Nothing needs to be installed for any of this: no Python, no runtime. Setup is
PowerShell, which Windows already has.

---

## Controls

Right hand aims and fires; the gun sits where your controller is.

| control | does |
|---|---|
| left stick | move |
| right stick | turn |
| right trigger | fire |
| X (left) | use / activate |
| Y (left) | reload |
| A (right) | jump |
| B (right) | duck |
| left grip | run |
| left trigger | flashlight |
| right stick click | weapon wheel: click, point with the stick, trigger or click to take |
| menu button (left) | the menu |
| left stick click, in the menu | photo mode: hides the menu so you can look around the paused world and take your own screenshot; click again to bring it back |
| hold the Meta button | recenter (also VR Options > Recenter) |

**On the Steam Frame** the left controller has a D-pad instead of X and Y, so
the buttons are laid out like a gamepad:

| control | does |
|---|---|
| A / B | jump / duck |
| Y / X | use / reload |
| D-pad left / right | previous / next weapon (the trigger takes it) |
| D-pad down | holster |
| D-pad up | quick save |
| right shoulder | next ammo type |
| Menu or View | the menu |

Triggers, grips, sticks and stick clicks work as on the Quest. In the menus
the D-pad moves the selection, A picks and B goes back.

**Change any button: VR Options > Controls.** Each controller keeps
its own map, and the page shows the one in your hands. A button only ever does
one job, so to swap two actions set one of them to (none) first. Reset to
defaults puts everything back. Pushing the right stick up or down can
be a button too: set Jump to Right stick up and Duck to Right stick down, as
many VR games do, and A and B are free for other actions. The fire trigger, the
grips, moving and turning, and the menu button stay where they are.

A red dot marks where the gun is pointing, on the surface or the body it will
hit. A green reticle in the middle of your view means there is something
there you can use: press X and it is used, even if your hand is pointing a
little off it. Through a scope, the shot leaves from the scope itself, so
anything you can see in it you can hit. Getting shot rumbles the controller on
the side the shot came from.

**Two hands on a pistol:** with a pistol out (P38, revolver, Contender, Luger),
bring your other hand to the gun and hold its grip, and a second hand appears on
the pistol's grip. While it is there, that grip does not make you run. It is
for the look and feel only; aiming is the same either way.

**Left-handed? VR Options > Hands and Weapons > Leftorium (left-handed)** swaps the hands: the gun
and its trigger go in your left hand (drawn as a left-handed gun), the
flashlight in your right, and the hand buttons above swap sides with them. The
sticks stay where they are (move on the left, turn on the right) unless you
also turn on Swap sticks, which works with or without the Leftorium. The
menu button stays where it is, and the handlebars on the vehicles work either
way.

**On the motorcycle and the snowmobile**

| control | does |
|---|---|
| X (left), near the vehicle | get on / get off |
| right trigger | throttle |
| left trigger | brake, and reverse when stopped |
| left stick left/right | steer, analog: a small push is a gentle turn |
| both grips, then turn your hands | steer with the handlebars, like a real bike |
| A (right) | wheelie (motorcycle only), one per press |

## Options

**Options** in the headset is short: VR Options first, then Game,
Performance and Sound. Display, Controls (keyboard, mouse and joystick),
Interface and Jukebox are behind **Show advanced options**. Open Options from
the pause menu or the main menu.

**VR Options** has Recenter and a row for each of these pages:

- **Turning and Moving:** snap or smooth turning, turn speed, swap sticks,
  Walk direction (Body, Head or Off hand: which way the stick's forward
  goes), Analog walking (how far you push the stick sets your speed),
  steering with both grips.
- **Your Body:** Show body and See yourself in mirrors (both on), head
  bob.
- **Hands and Weapons:** the Leftorium (left-handed play), whether the gun
  follows your hand, the gun's size, how far it follows your hand, weapon sway,
  controller rumble, and Cate's hands (on by default): her own gloved hands on your
  controllers, curled round the gun you hold, in the gloves of the outfit she
  is wearing (Setup makes those from your copy of the game).
- **Physical Play:** use the game with your body. The first row sets them all
  at once (Buttons or Physical); every button way keeps working either way.
  - **Holsters on the body:** grip at your gun-side hip for the pistol, over
    your gun-side shoulder for the long gun, at your chest for the current
    gadget, at the front of your belt for the coin and lipsticks. Grip at a
    holster again to put the weapon back. Holster height moves them all.
  - **Manual reload:** the reload button drops the clip; grip at the pouch on
    your other hip and bring the new clip to the gun.
  - **Swing to hit:** a fast punch lands. As in the original game, only a blow
    to the head of someone who has not seen you takes them down.
  - **Wrist display:** health, armor and ammo on the back of your off-hand
    wrist; turn it toward you like a watch to read it.
  - **Glasses to your eyes:** Cate's sunglasses (camera, mine detector,
    infrared) go on when you raise them to your eyes, and stay on when you
    lower your hand; the trigger then takes photos. Bring your hand back to your
    eyes and squeeze the grip to take them off.
  - **Throw by hand:** with the coin or a lipstick, hold the trigger, swing and
    let go. It flies the way your hand was moving, as hard as you threw it.
  - **Two hands on long guns:** with a rifle, SMG or shotgun, grip its front
    with your other hand and it aims from hand to hand, steadier. Open your
    hand to go back to one.
- **Screen and Aim:** the aim dot and its size, VR captions, large menu text,
  the steady desktop view, and Resolution %: how sharp the world is, as a
  percentage of your headset's own resolution (100 by default). It applies the
  next time you start the game; lower it if an older graphics card cannot hold
  90 fps.
- **Controls:** which button does what (see Controls above).

**On an omnidirectional treadmill** (Omni One, KAT Walk and the like), set
**Walk direction** to Head and turn Analog walking on. Forward then
follows wherever you face, turning your body turns you in the game, and your
pace sets your speed: walk slowly to creep, speed up to run. The right stick
still turns you if you want it to.

## What works, and what does not

**Works, confirmed in a headset:** the whole world in stereo at 90 fps, head
tracking, motion-controlled aiming and firing, scopes, the weapon wheel,
haptics, the motorcycle and the snowmobile, the HUD in both eyes, menus and
loading screens as a world-locked panel, doors, lifts, glass, characters and
their attachments, subtitles and the dialogue chooser, alt-tab, and a desktop
view for streaming or recording.

**New in 1.1, also confirmed in a headset:** your own body when you look down,
with legs that walk as you move; mirrors that reflect the room and show you in
them, holding your gun; the world drawn at your headset's full resolution; the
guns' metallic sheen; characters and objects lit by the lamps around them; a
second hand on pistols; the steady desktop view; the red dot centered in a scope;
and SteamVR headsets such as the Steam Frame.

**Known issues:**

- The game has not been played end to end in VR. Individual missions have.
- The controls are mapped for Meta Quest (Touch) controllers, which is what
  Virtual Desktop, Meta Quest Link and SteamVR-with-a-Quest all present, and
  for the Steam Frame's own controllers. Other controllers under SteamVR have no
  mapping of ours; SteamVR's own Controller Bindings screen can map them,
  untested.
- In Mastermind's fifth scene, two mirrors facing each other show each other
  as plain glass.
- Dialogue text could be larger; it is limited by the font sheet.
- Some lamps in dark interiors read dimmer than they should. This may already
  be fixed: several lighting settings were among the ones setup switches on
  (see below), and it has not been re-checked since.
- Multiplayer is untested and not a goal.

**Setup switches on features the game leaves off.** NOLF reads its options
through console variables, and one that does not exist reads as zero, so an
install whose Options pages have never been opened runs with gore, water
animation and the sky switched off, and nothing says so. The installer repairs
that from your own retail values and never changes a setting you have already
chosen. You can re-run it yourself:

```
powershell -ExecutionPolicy Bypass -File tools\repair-settings.ps1
```

**HD texture packs are not supported yet.** With the community's ESRGAN x4 pack
in `game\custom`, a level can take several minutes to load behind a frozen
loading screen, and the game can close while it loads. Keep `game\custom` empty
for now; support is planned for a later update.

**The main menu changes on a Friday.** That is not a fault and not ours: the
Modernizer this is built on picks a different main-menu layout by the date, a
casual one on Fridays and a winter one from 15 December. Same items, different
art. Add `-PlainMenu` to the launcher to pin the ordinary one.

**Testing aid.** From a command prompt in this folder,
`"Play No One Lives Forever VR.bat" -God` starts a session with god mode, every weapon and every
mission unlocked. It lasts for that session only.

Bug reports with a level name and a screenshot help the most.

---

## What this is built on

Every project this stands on:

- **Monolith Productions** made *No One Lives Forever* (2000), and released the
  **NOLF Source Code v1.003** in 2001. The rights today sit with
  Nightdive/Ziggurat.
- **[haekb/nolf1-modernizer](https://github.com/haekb/nolf1-modernizer)** is the
  modernized client this forked. It fixes the mouse, the framerate and the
  resolution handling that 2000 could not have anticipated. The VR work is a
  branch of that, and it would not have been startable without it.
- **[DR-89/fear-vr](https://github.com/DR-89/fear-vr)** is where the technique
  comes from: render the world twice per frame, once per eye, and leave the
  simulation alone. No line of its code is here (F.E.A.R. is a different engine
  and a different renderer), but the idea and its proof are theirs.

## Building from source

Nothing needs building to play: the release zip is complete. To build it
yourself:

1. Install Visual Studio 2019 Build Tools with the C++ desktop workload
   (the v142 toolset; the client and the renderer are 32-bit, the host 64-bit).
2. Clone this repository, then the game client into `src\nolf1-modernizer`:
   it is the `nolfvr` branch of the nolf1-modernizer fork on the same GitHub
   account as this repository.
3. Download the [OpenXR.Loader 1.1.61](https://www.nuget.org/packages/OpenXR.Loader/1.1.61)
   package from NuGet (it is a zip) and extract it into `libs\openxr`.
4. Run `tools\setup.ps1` once (it is what the release's `Setup.bat` runs) so
   there is a `game` folder to build into, then from PowerShell:
   `tools\build.ps1` (the client), `tools\build-renstub.ps1` (the
   renderer), `tools\build-vrhost.ps1` (the OpenXR host) and
   `tools\build-dinput-proxy.ps1`. `tools\make-release.ps1` packages a zip.

## License

The client shell is derived from the NOLF Source Code v1.003 and, like
nolf1-modernizer before it, remains bound by the EULA that the source release
was distributed under. That license is non-commercial: the source was put out
by Monolith in 2001 for people to modify their own copy of the game with, and
nothing here is offered on any broader basis.

Note: the `readme.txt` shipped with the v1.003
source lists `V. END USER LISCENCE AGREEMENT (EULA)` in its contents and then
prints the introduction again in its place, so that file does not actually
contain the terms, and neither this repository nor nolf1-modernizer carries a
copy of them. The obligation is real regardless (it was accepted when the
source was obtained), but if you need to read the text, get it from the
original distribution rather than from anything in this tree.

The renderer (`host/renstub`), the OpenXR host (`host/`) and the tools are our
own work, and are offered under the same terms, because a build of this project
is a derivative of the whole.

**Never sold, never monetized.** If you fork this, keep it that way.

No game data is included in this repository or in any release archive.

Two third-party files ship in the archive, `SDL2.dll` (zlib license) and the
OpenXR loader (MIT); their notices are in `THIRD-PARTY-NOTICES.md`.

---

**Get an email when the next port ships:** follow [Game Or Die on Patreon](https://www.patreon.com/cw/GameOrDie) for free. Ports are never paywalled.
