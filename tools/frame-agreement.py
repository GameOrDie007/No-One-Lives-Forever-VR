"""Does the correction the compositor applies equal the motion of the image?

That is the whole warping question, and it is a pure quaternion identity. It
needs no headset, no runtime and no game - only the two conventions, both of
which the engine has already printed and confirmed:

    LTRotation A * B  =  apply B in A's frame        (measured: yaw*pitch keeps
                                                      pitch as pitch)
    OpenXR -> LithTech: (x,y,z,w) -> (-x,-y,z,w)     (measured: XR pitch +30
                                                      gives LT forward +Y)

The compositor is handed an image, told the view pose P it was rendered from,
and at display time knows the view pose P2. It resamples by P^-1 * P2, expressed
in the rendered view's own frame.

The image was actually drawn from camera C and, had it been drawn at display
time, would have been drawn from C2. So the motion it should be corrected by is
C^-1 * C2, also in the rendered camera's frame.

Those two have to be the same rotation. Whatever they differ by IS the misdirected
correction - the amount the compositor rotates the picture in a direction the
scene never moved. Reported below in degrees.
"""

import itertools
import math

D2R = math.pi / 180.0
R2D = 180.0 / math.pi


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def qconj(q):
    return (-q[0], -q[1], -q[2], q[3])


def axis_angle(axis, deg):
    a = deg * D2R * 0.5
    s = math.sin(a)
    n = math.sqrt(sum(c * c for c in axis)) or 1.0
    return (axis[0] / n * s, axis[1] / n * s, axis[2] / n * s, math.cos(a))


def xr_head(yaw, pitch, roll):
    """What a headset reports: yaw * pitch * roll, right-handed, +Y up."""
    q = qmul(axis_angle((0, 1, 0), yaw), axis_angle((1, 0, 0), pitch))
    return qmul(q, axis_angle((0, 0, 1), roll))


def to_lt(q):
    """OpenXR -> LithTech. A Z-basis flip; also a quaternion homomorphism."""
    return (-q[0], -q[1], q[2], q[3])


def lt_euler(pitch, yaw, roll):
    """SetupEuler's order, checked against the engine: yaw then pitch then roll,
    each in the frame the previous ones left."""
    q = qmul(axis_angle((0, 1, 0), yaw), axis_angle((1, 0, 0), pitch))
    return qmul(q, axis_angle((0, 0, 1), roll))


def angle_between(a, b):
    d = qmul(qconj(a), b)
    w = max(-1.0, min(1.0, abs(d[3])))
    return 2.0 * math.acos(w) * R2D


def camera(mode, body_yaw, body_pitch, head_xr):
    h = to_lt(head_xr)
    body = lt_euler(body_pitch, body_yaw, 0.0)
    body_yaw_only = lt_euler(0.0, body_yaw, 0.0)
    if mode == 1:
        return qmul(body, h)              # head in the body's frame
    if mode == 2:
        return qmul(h, body)              # head in the world frame
    return qmul(h, body_yaw_only)         # mode 3: world frame, yaw only


def main():
    print("misdirected correction, in degrees, for one frame of head motion")
    print("(0.00 means the compositor turns the image exactly the way the "
          "scene moved)")
    print()

    # A head that is doing all three things at once, moving by a few degrees
    # between render and display - the 6-67 ms of staleness this pipeline
    # measures, at a brisk but ordinary head turn.
    poses = [
        ((20.0, -15.0, 6.0), (23.0, -13.0, 6.5)),
        ((-35.0, 22.0, -8.0), (-32.0, 20.0, -8.5)),
        ((5.0, 30.0, 0.0), (6.0, 31.5, 0.3)),
    ]
    bodies = [(0.0, 0.0), (45.0, 0.0), (90.0, 0.0), (180.0, 0.0),
              (270.0, 0.0), (270.0, 15.0), (135.0, -20.0)]

    print("%-14s %-8s %-8s %-8s" % ("body yaw/pitch", "mode 1", "mode 2", "mode 3"))
    worst = {1: 0.0, 2: 0.0, 3: 0.0}
    for by, bp in bodies:
        row = []
        for mode in (1, 2, 3):
            worst_here = 0.0
            for (h1, h2) in poses:
                q1 = xr_head(*h1)
                q2 = xr_head(*h2)
                c1 = camera(mode, by, bp, q1)
                c2 = camera(mode, by, bp, q2)
                r_true = qmul(qconj(c1), c2)

                # What the runtime applies. The layer's space cancels out of
                # this entirely: declaring in LOCAL or in a space rotated by
                # any T gives P^-1 P2 = (T^-1 H1)^-1 (T^-1 H2) = H1^-1 H2. The
                # yaw-carrying reference space cannot change this number, which
                # is why it is not a parameter of this table.
                r_app = to_lt(qmul(qconj(q1), q2))

                d = angle_between(r_true, r_app)
                worst_here = max(worst_here, d)
            row.append(worst_here)
            worst[mode] = max(worst[mode], worst_here)
        print("%-14s %-8.2f %-8.2f %-8.2f" % ("%.0f / %.0f" % (by, bp), *row))

    print()
    for mode in (1, 2, 3):
        print("VRQuatHead %d: worst misdirection %.2f deg" % (mode, worst[mode]))

    # And the axis question, which is the reported bug: feed a pure head pitch
    # and see what the camera does.
    print()
    print("a pure head pitch of +20, by body yaw, per mode "
          "(camera pitch / camera roll)")
    print("%-10s %-16s %-16s %-16s" % ("body yaw", "mode 1", "mode 2", "mode 3"))
    for by in (0.0, 45.0, 90.0, 180.0, 270.0):
        cells = []
        for mode in (1, 2, 3):
            c = camera(mode, by, 0.0, xr_head(0.0, 20.0, 0.0))
            fwd = rotate(c, (0.0, 0.0, 1.0))
            up = rotate(c, (0.0, 1.0, 0.0))
            pitch = math.asin(max(-1.0, min(1.0, fwd[1]))) * R2D
            hr = (fwd[2], 0.0, -fwd[0])
            n = math.hypot(hr[0], hr[2])
            roll = 0.0
            if n > 1e-4:
                d = (up[0] * hr[0] / n) + (up[2] * hr[2] / n)
                roll = math.asin(max(-1.0, min(1.0, d))) * R2D
            cells.append("%+6.1f / %+6.1f" % (pitch, roll))
        print("%-10.0f %-16s %-16s %-16s" % (by, *cells))


def rotate(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    # q * (v,0) * q^-1
    t = qmul(qmul(q, (vx, vy, vz, 0.0)), qconj(q))
    return (t[0], t[1], t[2])


if __name__ == "__main__":
    main()
