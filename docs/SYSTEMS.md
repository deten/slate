# Building systems

## DUCT

A run of duct in one command. It asks for three things and draws the rest
the way the office draws it:

```
DUCT
Enter system [Supply/Return/Exhaust] or how it is drawn [Line/Double] <Supply>: S
Enter size (width x depth, or diameter): 24x12
Specify start of run: 1200,400
Specify next point, or Enter to finish: 1200,1800
Specify next point, or Enter to finish: 2200,1800
Specify next point, or Enter to finish:
supply duct 24x12 drawn, 2 legs
```

- **The system** picks the layer. The drawing's own layer for that system
  is used when it has one, judged by its name the way the reading judges
  layers (HSDUCT, M-DUCT-SUPP and the like), and otherwise M-DUCT-SUPP,
  M-DUCT-RETN or M-DUCT-EXHS is made with the usual colour. A layer
  marked existing or demolition is never chosen for new work.
- **The size** sets the width. `24x12` is 24 wide and 12 deep, `12` is a
  round duct 12 across, in drawing units. The size is written along the
  longest leg of the run, just off the duct, reading along it and never
  upside down.
- **The route** is the centreline: points until Enter, two at least.

**Line** draws the run as one polyline with the duct's width, which is how
single-line plans are drawn. **Double** draws two polylines the duct's
width apart, offset from the centreline so the corners meet as they would
in a fitting. Say Line or Double before the system; Line is the default.

The size may be given first, in place of the system, when the system is
the default. All of it is one command, so it undoes as one.

Written by the assistant, a run is one line:

```
DUCT SUPPLY 24x12 1200,400 1200,1800 2200,1800
DUCT SUPPLY 12x10 1200,900 800,900
```

That is what keeps a duct layout short enough to read in the preview, and
right by construction: the layer, width and tag come from the command,
not from the model.
