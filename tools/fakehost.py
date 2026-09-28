"""A stand-in for nolfvr.exe that needs no headset, no runtime and no GPU.

The client only ever OPENS the shared block - the host creates it. So anything
that creates a block with the right magic, version and a ticking alive-stamp is
indistinguishable from the real host as far as the client is concerned, and
every VR path that is gated on VRShared::IsLive() will execute.

That turns most of this project's open questions into desk work:

  - the head-composition path runs against a real, moving, three-axis pose
    instead of a synthetic single-axis one
  - the frame marker is actually drawn, so it can be read off a screenshot and
    the decode checked end to end
  - whatever the client publishes back - screen size, rendered FOV, applied eye
    centres, body yaw - is visible here

It is NOT a substitute for the headset. It cannot tell you what anything looks
like. It tells you what the numbers are.

Usage:
    python fakehost.py                      moving three-axis head, 90 Hz
    python fakehost.py --static 30,-20,8    a fixed yaw,pitch,roll in degrees
    python fakehost.py --seconds 40         stop by itself
"""

import argparse
import ctypes
import math
import mmap
import os
import struct
import sys
import time

NAME = "Local\\NOLFVR_SharedState"
SIZE = 380      # v19: nHostPid appended after v18's recenter
O_HOST_PID = 376
MAGIC = 0x56464C4E
VERSION = 19

# Offsets, from VRShared.h. Every field is four bytes with four-byte alignment,
# which is the whole reason that header forbids anything else - the layout has
# to be identical in a 32-bit game and a 64-bit host, and now in this too.
O_MAGIC = 0
O_VERSION = 4
O_SEQUENCE = 8
O_FLAGS = 12
O_HEAD_YAW = 16
O_HEAD_PITCH = 20
O_HEAD_ROLL = 24
O_HEAD_POS = 28          # x,y,z
O_IPD = 40
O_FOV = 44               # left,right,up,down (radians)
O_FRAME = 60
O_ALIVE = 64
O_HANDS = 68             # 2 x 48 bytes
# Within one VRHandState. Version 14 appended the thumbstick, which is why
# every offset below the hands moved by 16.
H_STRIDE = 48
H_ACTIVE = 0
# THE POSE, which nothing here ever wrote. Both hands were marked ACTIVE by
# --stick and --buttons and then left at position (0,0,0) and orientation
# (0,0,0), which is not "no hand" - it is a hand at the origin pointing exactly
# where the head points. Every VR path gated on the controller ran, and the
# weapon it placed was indistinguishable from one placed by the view. So hand
# AIMING could not be tested at the desk at all, and the only report on it is
# that neither the hands nor the gun were visible, from a headset round.
H_POS = 4                # x,y,z in metres
H_YAW = 16
H_PITCH = 20
H_ROLL = 24
H_TRIGGER = 28
H_GRIP = 32
H_BUTTONS = 36
H_STICK_X = 40
H_STICK_Y = 44
O_SCREEN_W = 164
O_SCREEN_H = 168
O_GAME_FOV_X = 172
O_GAME_FOV_Y = 176
O_IN_MENU = 180
O_QUAT = 184             # x,y,z,w
O_POSE_LAG = 200
O_POSE_LAG_VALID = 204
O_EYE_CENTRE_YAW = 208   # [2]
O_EYE_CENTRE_PITCH = 216  # [2]
O_ASYM_ACTIVE = 224
O_APPLIED_YAW = 228      # [2]
O_APPLIED_PITCH = 236    # [2]
O_EXACT_POSE = 244
O_CALIB_ACTIVE = 248
O_CALIB_YAW = 252
O_HEAD_LOCKED = 256
O_BODY_YAW = 260
O_YAW_SPACE_MODE = 264
O_EYE_FOV_L = 268        # [2]
O_EYE_FOV_R = 276        # [2]
O_EYE_FOV_U = 284        # [2]
O_EYE_FOV_D = 292        # [2]
O_NATIVE_FRUSTUM = 300   # written by the RENDERER, read here

# Version 13: the shared eye texture. All written by the RENDERER; listed here
# so the fake host publishes a block big enough to hold them and so this file
# stays the single readable map of the contract.
O_EYETEX_SERIAL = 304
O_RECENTER_REQ = 368
O_RECENTER_GEN = 372
O_EYETEX_W = 308
O_EYETEX_H = 312
O_EYETEX_FORMAT = 316
O_ADAPTER_LUID_LO = 320
O_ADAPTER_LUID_HI = 324

R2D = 57.29577951308232
D2R = 1.0 / R2D

GetTickCount = ctypes.windll.kernel32.GetTickCount
GetTickCount.restype = ctypes.c_uint32


def quat_from_ypr(yaw_deg, pitch_deg, roll_deg):
    """The OpenXR-frame quaternion a headset would report for these angles.

    Right-handed, +Y up, -Z forward. Composed yaw * pitch * roll, which is the
    order the client's own decomposition assumes when it prints them back - so a
    round trip through the log should return the numbers put in here.
    """
    cy, sy = math.cos(yaw_deg * D2R * 0.5), math.sin(yaw_deg * D2R * 0.5)
    cp, sp = math.cos(pitch_deg * D2R * 0.5), math.sin(pitch_deg * D2R * 0.5)
    cr, sr = math.cos(roll_deg * D2R * 0.5), math.sin(roll_deg * D2R * 0.5)

    # q = qYaw * qPitch * qRoll
    def mul(a, b):
        ax, ay, az, aw = a
        bx, by, bz, bw = b
        return (
            aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz,
        )

    q = mul((0.0, sy, 0.0, cy), (sp, 0.0, 0.0, cp))
    q = mul(q, (0.0, 0.0, sr, cr))
    return q


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--static", default=None,
                    help="fixed yaw,pitch,roll in degrees instead of a sweep")
    ap.add_argument("--sway", type=float, default=0.0,
                    help="sway the head side to side by this many metres at "
                         "0.1 Hz, orientation held fixed. Two captures five "
                         "seconds apart differ only in head position.")
    ap.add_argument("--pos", default=None,
                    help="fixed head position x,y,z in metres, overriding the "
                         "neck model. Two runs differing ONLY in this prove "
                         "whether head translation reaches the picture: the "
                         "image must show parallax, near geometry moving more "
                         "than far.")
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="stop after this long (0 = run until killed)")
    ap.add_argument("--hz", type=float, default=90.0)
    ap.add_argument("--stick", default=None, metavar="X,Y",
                    help="hold the LEFT thumbstick at this position and mark "
                         "both controllers active, so the client's controller "
                         "mapping can be exercised with no headset. "
                         "--stick 0,1 walks forward.")
    ap.add_argument("--rstick", default=None, metavar="X,Y",
                    help="hold the RIGHT thumbstick. --rstick 1,0 turns right.")
    ap.add_argument("--rhand", default=None, metavar="YAW,PITCH,ROLL",
                    help="point the RIGHT controller this way, in degrees, and "
                         "mark both hands tracking. The client uses hand MINUS "
                         "head, so with --static 0,0,0 these are also the "
                         "angles relative to the view. NOTE THE SIGN: this is the "
                         "OpenXR frame, +Y up and -Z forward, so a POSITIVE "
                         "yaw rotates forward toward -X and points the hand "
                         "LEFT. --rhand -30,0,0 aims right. The old help "
                         "here said the opposite and cost an afternoon "
                         "nearly spent inverting working aim code, which "
                         "is the one thing that separates a weapon placed by "
                         "the HAND from one placed by the VIEW.")
    # A HAND THAT MOVES, because a hand that does not cannot show a lag.
    #
    # Every desk capture this harness has ever taken held the controller
    # perfectly still, and a whole class of bug is invisible to a still hand:
    # anything positioned from a pose one frame old is exactly right until
    # something turns. The muzzle flash was placed from LAST frame's gun pose
    # for weeks; three captures at three different fixed angles all looked
    # correct, and in the headset it slid off the barrel the moment the gun was
    # swung (19 September). The error is r * dTheta - zero when dTheta is zero.
    #
    # --rhand-sweep AMPLITUDE,HZ swings the right hand's yaw as a sine, so a
    # capture taken at any moment is a capture taken mid-turn. Peak angular
    # rate is 2*pi*HZ*AMPLITUDE degrees a second; "30,0.5" is 94 deg/s, about
    # what a person does swinging a rifle across a room.
    ap.add_argument("--rhand-sweep", default=None, metavar="AMP,HZ",
                    help="swing the right hand's yaw: amplitude in degrees, "
                         "frequency in Hz (e.g. 30,0.5). Reveals one-frame lag.")
    ap.add_argument("--rhand-pos", default=None, metavar="X,Y,Z",
                    help="where the RIGHT controller is, in metres, +Y up and "
                         "-Z forward. Default 0.2,1.3,-0.3 - roughly a hand "
                         "held out in front - because (0,0,0) is the floor "
                         "under your feet and puts the weapon there.")
    ap.add_argument("--lhand", default=None, metavar="YAW,PITCH,ROLL",
                    help="the same for the LEFT controller.")
    ap.add_argument("--lhand-pos", default=None, metavar="X,Y,Z",
                    help="where the LEFT controller is, in metres. Default is "
                         "--rhand-pos mirrored in X. With both positions given "
                         "the line between the hands is a HANDLEBAR: right "
                         "hand further back (+Z) than the left is a right turn.")
    ap.add_argument("--grip", action="store_true",
                    help="squeeze BOTH grips (analog 1.0) from the first frame "
                         "and mark both hands tracking. VRVehicleSteer 1 takes "
                         "the bars on this.")
    ap.add_argument("--stick-at", type=float, default=0.0, metavar="SECS",
                    help="hold --stick only from this many seconds in. A stick "
                         "held from frame 1 walks the player away from wherever "
                         "the run put them before the run has begun.")
    ap.add_argument("--stick-until", type=float, default=0.0, metavar="SECS",
                    help="release --stick at this many seconds in (0 = never). "
                         "With --stick-again this makes a walk, a pause and a "
                         "second walk: walk up to a vehicle, press X, ride.")
    ap.add_argument("--stick-again", type=float, default=0.0, metavar="SECS",
                    help="hold --stick again from this many seconds in.")
    ap.add_argument("--rstick-at", type=float, default=0.0, metavar="SECS",
                    help="hold --rstick only from this many seconds in.")
    ap.add_argument("--ipd", type=float, default=0.0621, metavar="METRES",
                    help="interpupillary distance the host reports. 0 puts both "
                         "eyes at the same point, so any left/right difference "
                         "in the picture is a BUG and not parallax.")
    ap.add_argument("--buttons", type=lambda s: int(s, 0), default=0,
                    metavar="MASK",
                    help="hold these VRBTN_ bits on BOTH hands: 1 trigger, "
                         "2 grip, 4 primary (A/X), 8 secondary (B/Y), "
                         "16 thumbclick. The client edge-triggers, so a held "
                         "button fires once - which is what makes this a "
                         "clean test of a menu activation.")
    ap.add_argument("--buttons-hand", default="both", choices=["both", "left", "right"],
                    help="which hand holds --buttons. 'both' is the old behaviour; "
                         "the weapon wheel is a RIGHT stick click and the pause "
                         "menu a LEFT one, so a both-hands click opens both.")
    ap.add_argument("--buttons-at", type=float, default=0.0, metavar="SECS",
                    help="press --buttons this many seconds in, instead of "
                         "from the first frame. The client edge-triggers, so a "
                         "button held from frame 1 fires its one edge during "
                         "the intro movie and never reaches the menu - which "
                         "reads exactly like the mapping not working.")
    ap.add_argument("--buttons-every", type=float, default=0.0, metavar="SECS",
                    help="with --buttons-at: PULSE the buttons - held for half of "
                         "every SECS period from --buttons-at on - so a client that "
                         "edge-triggers sees a fresh press each period (sweeping "
                         "the view past a target while pressing use).")
    ap.add_argument("--trigger", default=None, choices=["left", "right", "both"],
                    help="squeeze this hand's TRIGGER (analog 1.0 and the button bit) "
                         "from --trigger-at seconds in: the vehicle throttle/brake read "
                         "the analog value, which --buttons never wrote.")
    ap.add_argument("--trigger-at", type=float, default=0.0, metavar="SECS")
    ap.add_argument("--recenter-at", type=float, default=0.0, metavar="SECS",
                    help="at this many seconds in, act as the host does on a recenter: "
                         "bump nRecenterGen and re-origin the head 0.3 m LOWER from then on "
                         "(a sit-down: under the client's half-metre jump rule, so only the "
                         "generation can make it re-reference).")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--version", type=int, default=VERSION,
                    help="wire version to advertise; 9 is the 13 August client")
    # A HEADSET'S OWN FIELD, SYMMETRIC: half-angles in degrees, horizontal and
    # vertical, e.g. --eye-fov 59.4,61.3. NOT what a Quest 3 renders: the host
    # SUBMITS a symmetric bound like that, but the renderer draws each eye with
    # the runtime's own asymmetric frustum (about 54/40 across, 44 up, 55 down),
    # which is what the default below already is. For layouts judged in a real
    # headset's degrees, leave this off.
    ap.add_argument("--eye-fov", default=None, metavar="H,V",
                    help="symmetric per-eye half-angles in degrees for both eyes")
    args = ap.parse_args()

    # The block is always allocated at the CURRENT size and only the advertised
    # version changes. An older client maps sizeof(its own struct) bytes - 244
    # at version 9 against 252 today - and simply never looks at the tail, so
    # one block serves both. Every field an old client does read sits at the
    # same offset, because the contract only ever appends.
    if args.pos:
        args.pos = tuple(float(v) for v in args.pos.split(","))
        if len(args.pos) != 3:
            raise SystemExit("--pos wants three numbers: x,y,z in metres")
    version = args.version

    def triple(text, what):
        parts = [float(v) for v in text.split(",")]
        if len(parts) != 3:
            raise SystemExit("%s wants three numbers" % what)
        return parts

    rhand = triple(args.rhand, "--rhand") if args.rhand else None
    lhand = triple(args.lhand, "--lhand") if args.lhand else None
    rhandpos = (triple(args.rhand_pos, "--rhand-pos") if args.rhand_pos
                else [0.20, 1.30, -0.30])
    lhandpos = (triple(args.lhand_pos, "--lhand-pos") if args.lhand_pos
                else [-rhandpos[0], rhandpos[1], rhandpos[2]])
    # A position with no orientation still has to publish a hand, or the
    # client sees no controller at all.
    if args.lhand_pos and lhand is None:
        lhand = [0.0, 0.0, 0.0]
    if args.rhand_pos and rhand is None:
        rhand = [0.0, 0.0, 0.0]

    rstick = None
    if args.rstick:
        parts = [float(v) for v in args.rstick.split(",")]
        if len(parts) != 2:
            raise SystemExit("--rstick wants two numbers: x,y in -1..+1")
        rstick = (parts[0], parts[1])

    stick = None
    if args.stick:
        parts = [float(v) for v in args.stick.split(",")]
        if len(parts) != 2:
            raise SystemExit("--stick wants two numbers: x,y in -1..+1")
        stick = (parts[0], parts[1])

    rsweep = None
    if args.rhand_sweep:
        parts = [float(v) for v in args.rhand_sweep.split(",")]
        if len(parts) != 2:
            raise SystemExit("--rhand-sweep wants two numbers: amplitude,hz")
        rsweep = (parts[0], parts[1])

    static = None
    if args.static:
        parts = [float(v) for v in args.static.split(",")]
        while len(parts) < 3:
            parts.append(0.0)
        static = tuple(parts[:3])

    buf = mmap.mmap(-1, SIZE, tagname=NAME, access=mmap.ACCESS_WRITE)

    def put_u32(off, v):
        buf[off:off + 4] = struct.pack("<I", v & 0xFFFFFFFF)

    def put_f(off, v):
        buf[off:off + 4] = struct.pack("<f", v)

    def get_u32(off):
        return struct.unpack("<I", buf[off:off + 4])[0]

    def get_f(off):
        return struct.unpack("<f", buf[off:off + 4])[0]

    buf[0:SIZE] = b"\0" * SIZE
    put_u32(O_MAGIC, MAGIC)
    put_u32(O_VERSION, version)
    # Our own pid, so the game's host watchdog can be tested by ending this.
    put_u32(O_HOST_PID, os.getpid())
    # --ipd 0 makes BOTH EYES ONE POSE, which is the only honest control for
    # "do the two eyes disagree about anything except parallax". Without it a
    # left/right comparison measures parallax and proves nothing.
    put_f(O_IPD, args.ipd)
    # A Quest 3's left-eye frustum, in radians. The client only uses these for
    # the FOV it asks the renderer for, so the exact values matter less than
    # their being asymmetric and plausible.
    put_f(O_FOV + 0, -54.0 * D2R)
    put_f(O_FOV + 4, 40.0 * D2R)
    put_f(O_FOV + 8, 44.0 * D2R)
    put_f(O_FOV + 12, -55.0 * D2R)

    # The per-eye frustums, MIRRORED horizontally the way a real headset's
    # are: each eye sees further outward than inward. Deliberately not
    # symmetric and deliberately not equal to each other - a renderer that
    # silently used eye 0's frustum for both, or a symmetric one for
    # either, would look correct against any gentler stand-in.
    #
    # Vertical is the same for both eyes on a Quest, and left of centre is
    # DOWN 55 against UP 44, so the vertical asymmetry is real too.
    frusta = ((-54.0, 40.0, 44.0, -55.0), (-40.0, 54.0, 44.0, -55.0))
    if args.eye_fov:
        fh, fv = [float(v) for v in args.eye_fov.split(",")]
        frusta = ((-fh, fh, fv, -fv), (-fh, fh, fv, -fv))
        put_f(O_FOV + 0, -fh * D2R)
        put_f(O_FOV + 4, fh * D2R)
        put_f(O_FOV + 8, fv * D2R)
        put_f(O_FOV + 12, -fv * D2R)
        print("fake host: symmetric eye field +/-%.1f x +/-%.1f deg" % (fh, fv))
    for eye, (l, r, u, d) in enumerate(frusta):
        put_f(O_EYE_FOV_L + eye * 4, l * D2R)
        put_f(O_EYE_FOV_R + eye * 4, r * D2R)
        put_f(O_EYE_FOV_U + eye * 4, u * D2R)
        put_f(O_EYE_FOV_D + eye * 4, d * D2R)

    btn_hands = {"both": (0, 1), "left": (0,), "right": (1,)}[args.buttons_hand]
    if args.buttons and args.buttons_at <= 0.0:
        for h in (0, 1):
            put_u32(O_HANDS + h * H_STRIDE + H_ACTIVE, 1)
        for h in btn_hands:
            put_u32(O_HANDS + h * H_STRIDE + H_BUTTONS, args.buttons)
        print("fake host: holding buttons %#x on %s" % (args.buttons, args.buttons_hand))
    elif args.buttons:
        for h in (0, 1):
            put_u32(O_HANDS + h * H_STRIDE + H_ACTIVE, 1)
        print("fake host: buttons %#x will be pressed at %.1f s"
              % (args.buttons, args.buttons_at))

    if rstick is not None:
        for h in (0, 1):
            put_u32(O_HANDS + h * H_STRIDE + H_ACTIVE, 1)
        if args.rstick_at <= 0.0:
            put_f(O_HANDS + 1 * H_STRIDE + H_STICK_X, rstick[0])
            put_f(O_HANDS + 1 * H_STRIDE + H_STICK_Y, rstick[1])
        print("fake host: RIGHT STICK held at (%.2f, %.2f)" % rstick)

    if stick is not None:
        # Both hands active, left stick held. The client releases every
        # command it holds the moment neither hand is active, so a stick
        # value alone would do nothing.
        for h in (0, 1):
            put_u32(O_HANDS + h * H_STRIDE + H_ACTIVE, 1)
        if args.stick_at <= 0.0:
            put_f(O_HANDS + 0 * H_STRIDE + H_STICK_X, stick[0])
            put_f(O_HANDS + 0 * H_STRIDE + H_STICK_Y, stick[1])
        else:
            print("fake host: LEFT STICK will be held from %.1f s" % args.stick_at)

    if args.grip:
        for h in (0, 1):
            put_u32(O_HANDS + h * H_STRIDE + H_ACTIVE, 1)
            put_f(O_HANDS + h * H_STRIDE + H_GRIP, 1.0)
        print("fake host: both GRIPS squeezed")

    print("fake host: block published, %d bytes, version %d" % (SIZE, version))
    if stick is not None:
        print("fake host: LEFT STICK held at (%.2f, %.2f), both hands active"
              % stick)
    if static:
        print("fake host: STATIC head yaw %.1f pitch %.1f roll %.1f"
              % static)
    else:
        print("fake host: moving head - yaw, pitch and roll all non-zero and "
              "out of phase")
    sys.stdout.flush()

    seq = 0
    frame = 0
    recentered = False
    t0 = time.perf_counter()
    next_report = t0 + 1.0
    period = 1.0 / max(args.hz, 1.0)

    try:
        while True:
            now = time.perf_counter()
            t = now - t0
            if args.seconds > 0.0 and t >= args.seconds:
                break

            if static:
                yaw, pitch, roll = static
            else:
                # Three incommensurate rates, so no two axes are ever in phase
                # and the pose is never a pure rotation about one axis. A real
                # head is never a pure rotation about one axis either, and that
                # is exactly what every previous synthetic test assumed.
                yaw = 40.0 * math.sin(t * 0.83)
                pitch = 25.0 * math.sin(t * 1.27 + 0.7)
                roll = 10.0 * math.sin(t * 0.53 + 2.1)

            qx, qy, qz, qw = quat_from_ypr(yaw, pitch, roll)

            frame += 1
            seq += 1
            put_u32(O_SEQUENCE, seq * 2 - 1)     # odd: writing
            put_f(O_HEAD_YAW, yaw)
            put_f(O_HEAD_PITCH, pitch)
            put_f(O_HEAD_ROLL, roll)
            put_f(O_QUAT + 0, qx)
            put_f(O_QUAT + 4, qy)
            put_f(O_QUAT + 8, qz)
            put_f(O_QUAT + 12, qw)
            # A head that rotates also translates - the eyes are ~10 cm in
            # front of the neck pivot. The client applies this as of
            # 3 September; before that it used rotation only, which is what
            # made pitch warp (docs/HEAD-TRANSLATION.md).
            if args.sway > 0.0:
                # Orientation fixed, position swaying side to side. Two frames
                # half a period apart then differ ONLY in where the head is,
                # which is the one way to see at the desk whether head
                # translation reaches the picture - and it must show parallax,
                # near geometry moving further than far.
                px = args.sway * math.sin(t * 0.628)     # 0.1 Hz
                py, pz = 1.60, -0.10
            elif args.pos:
                px, py, pz = args.pos
            else:
                px = 0.10 * math.sin(yaw * D2R)
                py = 1.60 - 0.10 * (1.0 - math.cos(pitch * D2R))
                pz = -0.10 * math.cos(yaw * D2R)
            if args.recenter_at > 0.0 and t >= args.recenter_at:
                if not recentered:
                    recentered = True
                    put_u32(O_RECENTER_GEN, 1)
                    print("fake host: RECENTER at %.1f s - generation 1, head now 0.3 m lower" % t)
                py -= 0.3
            put_f(O_HEAD_POS + 0, px)
            put_f(O_HEAD_POS + 4, py)
            put_f(O_HEAD_POS + 8, pz)
            put_u32(O_FRAME, frame)
            # Pressed LATE, inside the same seqlock update as everything
            # else. Indented to match: an earlier version of this block
            # sat one level out and swallowed the alive stamp and the
            # seqlock close into its own body, so with no --buttons-at
            # the block was never published and the client ignored the
            # host entirely.
            if args.buttons and args.buttons_at > 0.0:
                el = time.perf_counter() - t0
                held = args.buttons if el >= args.buttons_at else 0
                if held and args.buttons_every > 0.0:
                    ph = ((el - args.buttons_at) % args.buttons_every) / args.buttons_every
                    held = args.buttons if ph < 0.5 else 0
                for h in btn_hands:
                    put_u32(O_HANDS + h * H_STRIDE + H_BUTTONS, held)
            if args.trigger:
                on = (time.perf_counter() - t0) >= args.trigger_at
                for h in {"left": (0,), "right": (1,), "both": (0, 1)}[args.trigger]:
                    put_f(O_HANDS + h * H_STRIDE + H_TRIGGER, 1.0 if on else 0.0)
            if rstick is not None and args.rstick_at > 0.0:
                on = (time.perf_counter() - t0) >= args.rstick_at
                put_f(O_HANDS + 1 * H_STRIDE + H_STICK_X, rstick[0] if on else 0.0)
                put_f(O_HANDS + 1 * H_STRIDE + H_STICK_Y, rstick[1] if on else 0.0)
            if stick is not None and args.stick_at > 0.0:
                tt = time.perf_counter() - t0
                on = tt >= args.stick_at
                if args.stick_until > 0.0 and tt >= args.stick_until:
                    on = (args.stick_again > 0.0 and tt >= args.stick_again)
                put_f(O_HANDS + 0 * H_STRIDE + H_STICK_X, stick[0] if on else 0.0)
                put_f(O_HANDS + 0 * H_STRIDE + H_STICK_Y, stick[1] if on else 0.0)
            # THE HANDS, inside the same seqlock update as the head. A pose
            # written outside it can be read half-updated, and a half-updated
            # hand is a weapon that jumps once a second for no reason anybody
            # would connect to this file.
            for idx, hnd in ((0, lhand), (1, rhand)):
                if hnd is None:
                    continue
                base = O_HANDS + idx * H_STRIDE
                put_u32(base + H_ACTIVE, 1)
                # The sweep is applied to the RIGHT hand only, and only to yaw:
                # one axis moving is enough to expose a lag and keeps the
                # capture readable.
                fYaw = hnd[0]
                if rsweep is not None and idx == 1:
                    tt = time.perf_counter() - t0
                    fYaw += rsweep[0] * math.sin(2.0 * math.pi * rsweep[1] * tt)
                put_f(base + H_YAW, fYaw)
                put_f(base + H_PITCH, hnd[1])
                put_f(base + H_ROLL, hnd[2])
                # The left hand mirrors the right unless --lhand-pos placed
                # it; with both placed, the pair is a handlebar.
                hp = rhandpos if idx == 1 else lhandpos
                put_f(base + H_POS + 0, hp[0])
                put_f(base + H_POS + 4, hp[1])
                put_f(base + H_POS + 8, hp[2])
            put_u32(O_ALIVE, GetTickCount())
            put_u32(O_SEQUENCE, seq * 2)         # even: done

            if not args.quiet and now >= next_report:
                next_report = now + 1.0
                print("  frame %6d  head y%+7.1f p%+7.1f r%+7.1f | client: "
                      "screen %ux%u fov %.1fx%.1f menu %u exact %u asym %u "
                      "bodyYaw %+.1f yawSpace %u"
                      % (frame, yaw, pitch, roll,
                         get_u32(O_SCREEN_W), get_u32(O_SCREEN_H),
                         get_f(O_GAME_FOV_X) * R2D, get_f(O_GAME_FOV_Y) * R2D,
                         get_u32(O_IN_MENU), get_u32(O_EXACT_POSE),
                         get_u32(O_ASYM_ACTIVE), get_f(O_BODY_YAW) * R2D,
                         get_u32(O_YAW_SPACE_MODE)))
                sys.stdout.flush()

            sleep = period - (time.perf_counter() - now)
            if sleep > 0:
                time.sleep(sleep)
    except KeyboardInterrupt:
        pass

    print("fake host: stopping after %d frames" % frame)


if __name__ == "__main__":
    main()
