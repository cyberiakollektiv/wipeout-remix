# Roadmap
Somewhere I can keep my thoughts about what I'm currently working on.

## Vibration
Currently requires linking ship.c, ship_player.c and weapon.c to platform_sdl and platform_sdl3. The current idea for how vibration should feel is like this:
- [ ] Heavy rumble when crashing based on collision speed or getting hit by explosives
- [ ] Missile & rocket send out one heavy rumble on launch, mines vibrate 3x at medium strength, turbo makes the controller consistently vibrate intensely until it's over, and shield gently whirrs the motors until it's deactivated.

## Camera
The current build has the following new camera options:
- [x] Cockpit (shows the front of the ship)
- [x] Behind (shows behind the ship)
- [x] Top-down (shows the track from a top-down perspective)
- [x] Modern look with right-analog stick

They currently need to be integrated slightly better, the current prototype is that right-stick looks around like a more "modern" video game, and holding back on the right-stick shows the behind view, and the regular camera button cycles between external view, cockpit view, internal view and top down view.

## Renderer
For modernization efforts, Vulkan and Metal renderers should probably be written for newer operating systems.

## Audio
- [ ] Eventually implement basic reverb when there are more than 4 track faces.

## Minimap
- [x] Basic minimap support has been added into race.c via map.c and map.h. It's not pretty, but it works perfectly and uses pilot icons from pilot.cmp. It needs better positioning, slightly better integration with UI/resolution scaling, and ideally, better emblem icons with transparent backgrounds.
- [ ] Proof of concept minimap test on a dual-screen device such as a (3)DS or a Wii U.

## 2097 / 64 Support
- [x] Currently supports 2097 / 64 tracks AND soundtrack files if installed in adjacent "wipeout2" and "wipeout64" folders. Wipeout 3 soundtrack's soundtrack is also supported if converted to .qoa (but the track files are completely different as WO1 + 2097/XL share an engine and WO3 doesn't). Soundtracks are automatically matched to the launched circuit's game type, ie; Wipeout 2097/XL maps will automatically play 2097/XL songs unless manually changed. 
- [x] Wipeout 3 soundtrack's soundtrack is also supported if converted to .qoa (but the race circuit files are completely different as WO1 + 2097/XL share an engine and WO3 doesn't).
- [ ] 2097/XL weapon and HUD support may be added at some point down the line.
