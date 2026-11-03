#!/usr/bin/env python3
"""Generate long input scripts for the memory/stability soaks (heap track).

  tools/gen_soak.py qrloop [N]      N Quick Race loads (default 20): race types
                                    Circuit, Sprint, Drift, Street X, URL in
                                    turn, ~15 s of driving, pause -> Quit to
                                    Main Menu.  "status load_K" after each.
  tools/gen_soak.py roam [MIN]      new career profile -> free roam with the
                                    game's autopilot for MIN minutes (default
                                    20); "status roam_K" every minute.
  tools/gen_soak.py resetloop [N]   N display-mode round trips (Options ->
                                    Display: one row down, apply, back to the
                                    native row, apply = 2 D3D Resets each).
  tools/gen_soak.py soak [MIN]      mixed soak for MIN minutes (default 95):
                                    career free roam (autopilot), quit to main
                                    menu, Quick Race loads of every type with
                                    restarts and quits, and a display-mode
                                    change (Options -> Display, D3D Reset)
                                    about every 10 minutes.

The script language is runtime/autotest.c's.  Frame counts assume ~120 fps
(the game caps there); everything that matters is synchronised on the
front-end package or the GameFlow state, not on time.
"""
import sys

QR_TYPES = [(0, "circuit"), (1, "sprint"), (3, "drift"), (4, "streetx"), (5, "url")]

def boot_no_profile(o):
    o.append("0     tapuntil RETURN MC_Background.fng 120 4000   # title -> profile prompt")
    o.append("+200  tap RETURN                                   # \"No\"")
    o.append("+0    waitfng GarageMain.fng 3000")
    o.append("+300  status menu_0")

def quit_to_menu(o, finished_ok=True):
    # pause -> last item (Quit to Main Menu; the carousel does not wrap) -> OK
    o.append("+0    tap ESCAPE")
    o.append("+120  tap RIGHT")
    for _ in range(5):
        o.append("+30   tap RIGHT")
    o.append("+60   tap RETURN")
    o.append("+120  tap LEFT")
    o.append("+60   tap RETURN")
    o.append("+0    waitfng GarageMain.fng 1500")
    # if the race had already ended (results screens), back out with ESC.
    # UI_Main, not GarageMain: GarageMain stays the top package on every
    # front-end page (Options included)
    o.append("+0    tapuntil ESCAPE UI_Main.fng 150 2400")

def menu_select(o, idx):
    # main menu carousel (Career, Quick Race, Online, LAN, Customize, Profile
    # Manager, Options: idx 0..6; Split Screen after Quick Race since
    # runtime/splitscreen.c); it remembers its position and does not wrap
    o.append("+0    tapuntil ESCAPE UI_Main.fng 150 2400")
    o.append("+300  tap LEFT")
    for _ in range(6):
        o.append("+40   tap LEFT")
    for _ in range(idx + (idx >= 2)):
        o.append("+40   tap RIGHT")
    o.append("+90   log main menu item %d" % idx)

def qr_race(o, k, label, drive=1800, restart=False):
    menu_select(o, 1)
    o.append("+0    tapuntil RETURN UI_QRModeSelect.fng 150 3000 # main menu -> Quick Race")
    o.append("+200  tap LEFT")
    for _ in range(5):
        o.append("+20   tap LEFT")                     # back to Circuit (no wrap)
    for _ in range(k):
        o.append("+30   tap RIGHT")
    o.append("+60   tapuntil RETURN PC_Loading.fng 120 4000      # track, options, car")
    o.append("+0    waitmem 0x8654a4 6 4000")
    o.append("+300  status race_%s" % label)
    o.append("+0    down UP")
    seg = drive // 3
    o.append("+%d  tap RIGHT 30" % seg)
    o.append("+%d  tap LEFT 30" % seg)
    o.append("+%d  up UP" % seg)
    o.append("+0    dump %s" % label)
    if restart:
        # pause -> Restart (second item) -> confirm
        o.append("+0    tap ESCAPE")
        o.append("+120  tap RIGHT")
        o.append("+60   tap RETURN")
        o.append("+120  tap LEFT")
        o.append("+60   tap RETURN")
        o.append("+0    waitmem 0x8654a4 6 4000")
        o.append("+300  status restart_%s" % label)
        o.append("+0    down UP")
        o.append("+600  up UP")
    quit_to_menu(o)
    o.append("+300  status load_%s" % label)

def career_roam(o, minutes, tag):
    # new profile, Start New Career, SMS, free roam with autopilot
    o.append("+0    tapuntil RETURN MC_Background.fng 120 4000")
    o.append("+120  tap LEFT")
    o.append("+30   tap RETURN")
    o.append("+240  tap BACK")
    for _ in range(3):
        o.append("+8    tap BACK")
    o.append("+20   type SOAK")
    o.append("+60   tap RETURN")
    o.append("+240  tap RETURN")
    o.append("+0    waitfng GarageMain.fng 3000")
    o.append("+300  tap LEFT")
    o.append("+60   tap RETURN                      # Career")
    o.append("+300  tap RETURN                      # Start New Career")
    o.append("+0    waitfng UI_EngageMessageDialog.fng 6000")
    o.append("+300  tap RETURN                      # SMS: Continue")
    o.append("+0    waitfng HUD_SingleRace.fng 3000")
    o.append("+600  status roam_start")
    roam_minutes(o, minutes, tag)

def display_change(o, label):
    # Options -> Display -> resolution one row down and back up (two Resets)
    menu_select(o, 6)
    o.append("+0    tap RETURN                                   # Options")
    # categories Audio, Display, Gameplay, Player, Controls, EA Trax, Credits:
    # remembered, no wrap
    o.append("+300  tap LEFT")
    for _ in range(5):
        o.append("+40   tap LEFT")
    o.append("+40   tap RIGHT                                    # Display")
    o.append("+90   tap RETURN")
    o.append("+300  tap LEFT                                     # resolution row: one down")
    o.append("+90   tap ESCAPE                                   # leave -> apply?")
    o.append("+200  tap RETURN")
    o.append("+400  status reset_%s_a" % label)
    o.append("+0    tap RETURN")
    o.append("+300  tap RIGHT                                    # back to the native row")
    o.append("+90   tap ESCAPE")
    o.append("+200  tap RETURN")
    o.append("+400  status reset_%s_b" % label)
    o.append("+0    dump reset_%s" % label)
    o.append("+0    tapuntil ESCAPE UI_Main.fng 150 2400")

def qrloop(n):
    o = ["# Generated by tools/gen_soak.py qrloop %d: memory growth per race load." % n]
    boot_no_profile(o)
    for i in range(n):
        k, nm = QR_TYPES[i % len(QR_TYPES)]
        qr_race(o, k, "%d_%s" % (i + 1, nm))
    o.append("+60   quit")
    return o

def roam(minutes):
    o = ["# Generated by tools/gen_soak.py roam %d: career free roam with the game's autopilot." % minutes]
    career_roam(o, minutes, "roam")
    o.append("+300  status roam_end")
    o.append("+60   quit")
    return o

def resetloop(n):
    o = ["# Generated by tools/gen_soak.py resetloop %d: Metal memory across D3D Reset." % n]
    boot_no_profile(o)
    for i in range(n):
        display_change(o, "%d" % (i + 1))
        o.append("+300  status after_reset_%d" % (i + 1))
    o.append("+60   quit")
    return o

def roam_minutes(o, minutes, tag):
    o.append("+0    autopilot on")
    for m in range(minutes):
        o.append("+60000ms status %s_%d" % (tag, m + 1))
        if m % 5 == 4:
            o.append("+0    dump %s_%d" % (tag, m + 1))
        # an SMS or mailbox may have popped up (it pauses the world): back out
        # of it; no key is sent when the race HUD is on top
        o.append("+0    tapuntil ESCAPE HUD_SingleRace.fng 120 1200")
        o.append("+60   autopilot on                  # again, in case an event took the car back")
    o.append("+0    autopilot off")

def career_resume(o, minutes, tag):
    # main menu -> Career -> Resume Career -> free roam (autopilot)
    menu_select(o, 0)
    o.append("+0    tap RETURN                                   # Career")
    o.append("+300  tap RETURN                                   # Resume Career")
    o.append("+0    waitfng HUD_SingleRace.fng 4000")
    o.append("+600  status %s_start" % tag)
    roam_minutes(o, minutes, tag)

def soak(minutes):
    o = ["# Generated by tools/gen_soak.py soak %d: mixed long-session soak." % minutes]
    # 1) new career profile, free roam ~10 min, quit to the main menu
    career_roam(o, 10, "roam")
    quit_to_menu(o)
    o.append("+300  status career_quit")
    # 2) cycles (~13 min each): 2 x (5 Quick Race loads of every type, every
    #    4th with a restart, then a display-mode change = two D3D Resets),
    #    3 minutes of career free roam, quit to the main menu
    t = 12.0
    i = 0
    cyc = 0
    while t < minutes - 2:
        cyc += 1
        for blk in range(2):
            for k, nm in QR_TYPES:
                i += 1
                qr_race(o, k, "%d_%s" % (i, nm), restart=(i % 4 == 0))
            display_change(o, "%d%s" % (cyc, "ab"[blk]))
        career_resume(o, 3, "roam%d" % cyc)
        quit_to_menu(o)
        o.append("+300  status cycle_%d" % cyc)
        t += 13.5
    o.append("+300  status soak_end")
    o.append("+60   quit")
    return o

if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "qrloop"
    arg = int(sys.argv[2]) if len(sys.argv) > 2 else None
    lines = {"qrloop": lambda: qrloop(arg or 20), "roam": lambda: roam(arg or 20),
             "resetloop": lambda: resetloop(arg or 10), "soak": lambda: soak(arg or 95)}[what]()
    print("\n".join(lines))
