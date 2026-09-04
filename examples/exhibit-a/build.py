"""Build Exhibit A from the hand sketch, in Slate, and save it as DWG.

Text boxes are sized for other CAD software's txt.shx, which is wider than the face
Slate substitutes for it -- four labels wrapped in other CAD software and not here.
A wider box only moves the wrap point, so nothing moves in either.

Every coordinate is in inches, measured from the property line: positive x
into Lot 35, positive y up the sheet. The sketch is dimensioned but not to
scale, so the dimensions govern and the picture only sets the proportions.
"""
import subprocess, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = os.path.join(ROOT, "build", "RelWithDebInfo", "bin", "Slate.exe")
HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE = os.path.join(HERE, "template.dxf")

# Traced from the full-resolution scan at 5.74 px/in, in inches from the
# property line (x) and from where the sidewalk crosses it (y). The dimensioned
# values are exact; everything else is the sketch's own proportions.
import math

# The sidewalk is a gentle circular curve, not a straight line: fitting its ink
# gives a 62 ft radius that bows the line up 11" over its length. Fixed as
# centre and radius, so every point below sits on the same curve.
SW_CENTRE = (358.4, -653.5)
SW_RADIUS = 746.0
def sidewalk_y(x):
    return SW_CENTRE[1] + math.sqrt(SW_RADIUS**2 - (x - SW_CENTRE[0])**2)
def sidewalk_at(angle_deg):
    a = math.radians(angle_deg)
    return (SW_CENTRE[0] + SW_RADIUS*math.cos(a), SW_CENTRE[1] + SW_RADIUS*math.sin(a))
SIDEWALK = [sidewalk_at(123.65), sidewalk_at(113.8), sidewalk_at(103.97)]

# The bottom-left corner is scooped: the wall's left face stops at its foot
# and an arc leaves it horizontally, curving down to meet the sidewalk at 45
# degrees. Centred straight below the foot, so it starts tangent to level.
FOOT = (-48.0, 10.5)
CORNER_RADIUS = 52.9
CORNER_CENTRE = (FOOT[0], FOOT[1] - CORNER_RADIUS)
def corner_at(angle_deg):
    a = math.radians(angle_deg)
    return (CORNER_CENTRE[0] + CORNER_RADIUS*math.cos(a), CORNER_CENTRE[1] + CORNER_RADIUS*math.sin(a))
def corner_end():
    # Where the corner arc meets the sidewalk circle.
    (cx, cy), (sx, sy) = CORNER_CENTRE, SW_CENTRE
    d = math.hypot(sx-cx, sy-cy)
    a = (CORNER_RADIUS**2 - SW_RADIUS**2 + d*d) / (2*d)
    h = math.sqrt(CORNER_RADIUS**2 - a*a)
    mx, my = cx + a*(sx-cx)/d, cy + a*(sy-cy)/d
    candidates = [(mx + h*(sy-cy)/d, my - h*(sx-cx)/d), (mx - h*(sy-cy)/d, my + h*(sx-cx)/d)]
    return max(candidates, key=lambda p: p[0])   # the one at the wall, not far out on Lot 36
CORNER_END = corner_end()
CORNER_MID = corner_at((90 + math.degrees(math.atan2(CORNER_END[1]-CORNER_CENTRE[1], CORNER_END[0]-CORNER_CENTRE[0]))) / 2)

# The wall outline, open at the bottom because the sidewalk is its bottom edge
# in the sketch. Clockwise: up the far-right face from the sidewalk, along the
# top, down the left face to the foot, where the corner arc takes over.
WALL = [(150, sidewalk_y(150)), (150, 92), (149.5, 93.5), (146, 96), (138, 98), (120, 102),
        (103, 111), (88, 122), (72, 134), (60, 149), (55, 162), (52, 172), (50, 180),
        (44, 212), (40, 278),
        (-14, 278),
        (-14, 134), (-18, 118), (-23, 102), (-30, 89), (-36, 80), (-42, 73), (-46, 66),
        (-48, 57), FOOT]

def script(out):
    a = ["-platform","offscreen","--no-recovery-prompt","--open",TEMPLATE]
    def run(*xs):
        for x in xs: a.extend(["--run", str(x)])

    run("LAYER","C-PROP")
    run("LINE","0,-45","0,305","")

    run("LAYER","L-WALL")
    # One polyline, corner arc included: the arc is a segment through a
    # second point, so it turns the way the sketch turns it -- down from the
    # foot of the left face into the sidewalk.
    run("PLINE")
    for x,y in WALL: run(f"{x:.2f},{y:.2f}")
    run("A", "S", f"{CORNER_MID[0]:.2f},{CORNER_MID[1]:.2f}",
        f"{CORNER_END[0]:.2f},{CORNER_END[1]:.2f}")
    run("")

    run("LAYER","C-SWLK")
    run("ARC", *[f"{x:.2f},{y:.2f}" for x,y in SIDEWALK])

    run("LAYER","A-ANNO-DIMS")
    # Separate lines: 14" cannot hold 7" text between two 5" arrows, so
    # other CAD software moves that text outside the extension lines -- where it
    # collided with the 40" text when both shared one line.
    run("DIMLINEAR","0,278","40,278","20,264")
    run("DIMLINEAR","-14,278","0,278","-7,242")
    run("DIMLINEAR","0,178","52,178","26,178")
    run("DIMLINEAR","0,84","150,84","75,84")
    run("DIMLINEAR","-48,34","0,34","-24,34")

    run("LAYER","A-ANNO-LEAD")
    run("LEADER","30,278","62,306","96,306","","WALL")
    run("LEADER","95,117","190,152","240,152","","PLANTER WALL")
    run("LEADER","-19,115","-95,150","-145,150","","PLANTER WALL")

    run("LAYER","A-ANNO-TEXT")
    # Rotated labels, as the sketch has them: PROPERTY LINE reads up the line
    # itself, inside the wall; SIDEWALK runs along the sidewalk.
    run("TEXT","-11,160","7","90","PROPERTY LINE","")
    run("TEXT","80,46","9","22","SIDEWALK","")
    # Underlined with MTEXT's own code rather than a drawn line, so each program
    # rules exactly the width of its own text -- a line sized for one font is
    # wrong in the other.
    run("MTEXT","180,196","470,214","12",r"\LLOT 35 TRACT 6922\l")
    run("MTEXT","-230,44","60,62","12",r"\LLOT 36 TRACT 6922\l")
    run("MTEXT","8,-34","60,-20","11","PL")
    run("MTEXT","-70,-92","330,-64","22",r'\LEXHIBIT "A"\l')
    run("MTEXT","-120,-132","380,-104","22",r"\LLANDSCAPE AGREEMENT\l")

    run("ZOOMEXTENTS")
    return a + ["--save-as", out]

def build(out):
    r = subprocess.run([EXE] + script(out), capture_output=True, text=True,
                       timeout=600, cwd=ROOT)
    log = r.stdout + r.stderr
    bad = [l for l in log.splitlines() if l.startswith("\u2717") or l.startswith("!")]
    return log, bad

if __name__ == "__main__":
    for name in sys.argv[1:]:
        log, bad = build(os.path.join(HERE, name))
        print(name, "->", "PROBLEMS: " + str(bad) if bad else "clean")
        print("   ", log.strip().splitlines()[-1])
