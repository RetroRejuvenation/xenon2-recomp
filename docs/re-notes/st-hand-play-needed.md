# Atari ST: what only a hand-played session can settle

Everything the scripted replays can reach is in `atari-st-replays.md`. These are the questions left that need a person at the keyboard (or a joystick) on their own copy of the Kixx disk. None of them
needs a code change first; each says what to look at and what the answer decides.

Start the game as usual with `x2recomp --game st --st-disk <your Kixx .st>`. To record what happened, add `--st-watch A06,E04,CD0,42E,CDA,C92,CC6,CCA` (it prints a line each time one of those engine
words changes, with the frame number), or just note the level, the scroll behaviour and the score on screen.

1. **Does level 2 stall in real play?** In the scripted runs the level 2 scroll stops for good (ours at `$AE8` or `$1A2`, Hatari at `$F78` or `$1AB`; the push-back counter `$42E` and the scroll step `$CDA`
   flip between 1 and -1 and the ship ignores the stick) but the energy and ships were poked to full every frame, so the ship can never die. Play level 2 with no cheats (Backspace god mode off) to the end.
   - It scrolls on to the boss: the stall is an artefact of the invincible ship and `atari-st-replays.md` section 6 can say so.
   - It stops at the same places: note the scroll position (`$CD0` in the watch output), whether the ship sits on scenery, and whether dying or shooting something frees it. That would mean a game rule the
     scripts do not meet (something that must be destroyed before the scroll goes on), and the 24 chained objects of types `$114`-`$11C` that sit above the screen there are the first suspects.
2. **Boss 2 (the spider) and boss 4 (the head) defeated by shots.** Their death code is mapped (`$50A7C`, `$4FBB2`) but no script has ever hit them. Play to each boss and kill it: `A06` going to `FF` and
   the end-of-level shop confirms the path, then the next level loading. If the spider never moves when you arrive, that is the same stall as item 1.
3. **Boss 3 defeated by shots.** `boss3_done` sets the two death flags (`$D92`, `$D93`) itself; a real kill should set them both (they are set by the parts' death routines at `$4F800` and after). Note which
   parts you had to destroy before the level ended.
4. **The mid-level shop on levels 2-4.** None was ever requested in a script (level 1's comes at `$CD0` = `$9BF`, level 5's about `$638`). If a real run shows one, note the scroll position.
5. **Anything audible that the scripted runs cannot judge**: the music and the effects across a whole level against a real ST (or Hatari with sound), and the front end's 1.3% fast animation
   (`atari-st-video.md`).
