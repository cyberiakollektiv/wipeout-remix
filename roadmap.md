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

They currently need to be integrated slightly better, the current prototype is that right-stick looks around like a more "modern" video game, and holding back on the right-stick shows the behind view, and the regular camera button cycles between external view, cockpit view, internal view and top down view. Currently there are no menu options for any of these camera options, which should be rectified (RS should be bound to a menu toggle for "classic look" and "modern look")

## Renderer
For modernization efforts, Vulkan and Metal renderers should probably be written for newer operating systems.

## Audio
- [ ] Eventually implement basic reverb when there are more than 4 track faces.

## Minimap
- [x] Basic minimap support has been added into race.c via map.c and map.h. It's not pretty, but it works perfectly and uses pilot icons from pilot.cmp. It needs better positioning, slightly better integration with UI/resolution scaling, and ideally, better emblem icons with transparent backgrounds.
- [x] Proof of concept minimap test on a dual-screen device such as a (3)DS or a Wii U.

## 2097 / 64 Support
There's already a ton of code referencing 2097 and 64 that's not currently used anywhere. Once the bulk of the main fixes are made, I'll look into integrating support for 2097 and 64 tracks (not as ports of their own games, but selectable tracks in wipeout-rewrite's wipEout 1 engine). Currently, 2097 and 3 soundtracks are supported as long as you port each soundtrack file to .QOA and place it in the "wipeout2097" and "wipeout3" folders under "music". 
