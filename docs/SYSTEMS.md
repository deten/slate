# Drawing the way the drawing is drawn

Every office draws its things its own way: a duct run as one wide line or
as two lines its width apart, a thermostat as a letter in a circle, a
fixture as a block with a tag. Those ways are already in the drawing, and
in the office's other drawings. Slate does not hold a command for each
kind of thing; it holds one command that draws another one like an example.

## LIKE

**LIKE** (alias SAME) takes an example and what is new about the copy, and
makes a new object in the example's style:

```
LIKE
Select the example to draw another like: (pick a duct run)
Enter the tag for the new run, or Enter to keep the example's: 12x10
Specify start of run: 1200,900
Specify next point, or Enter to finish: 800,900
Specify next point, or Enter to finish:
Drawn like 3F2A: 1 leg, tagged 12x10
```

What comes across from the example:

- its layer, colour, linetype and lineweight;
- for a run: its width when it is a wide polyline, or its **second line**
  when it is drawn double, judged by the parallel line on the same layer
  the same distance away;
- the **tag** beside a run, with its text style, height, layer and
  placing, put on the longest leg of the new run, reading along it and
  never upside down. The new tag's text is what you give; Enter keeps the
  example's;
- for a block: the block, its scale and rotation, and its attributes with
  their values, the first taking the new text if you give one;
- for a letter in a circle: the circle and the letter, the letter changed
  to the new text if you give one;
- for text: the style, height, rotation and justification.

A run is a polyline, line or arc; give the route's points until Enter. A
block, circle or text is placed at one point. Pick the example first and
then start the command, or select it when asked, or type its handle. A
hatch, a dimension or a leader cannot be copied this way, and LIKE says so.

## What the assistant does with it

The reading of the drawing lists one example of each kind of thing it
draws, by handle: the longest run of each system layer, one symbol of
each tag, one door and one window block, one room label. The list goes
to the assistant with the drawing, and **ASK EXAMPLES** gives it in full.
The assistant then writes a run as one line:

```
LIKE 3F2A 24x12 1200,400 1200,1800 2200,1800
LIKE 5B10 T 1214,1342
```

So the model decides the route and the size, and the drawing decides how
a run of that kind looks. That is what keeps a layout the model writes
short, readable in the preview, and in the office's style without the
office's style being written into Slate. An electrical, plumbing or
controls drawing gets the same treatment from the same command, because
nothing in it knows what a duct is.

## DUCT

For a drawing with nothing to copy yet, **DUCT** draws a run of duct from
the system, the size and the route: a wide polyline on the drawing's own
layer for that system, or M-DUCT-SUPP, M-DUCT-RETN or M-DUCT-EXHS made
with the usual colour, with the size written along the longest leg.
**Double** draws it as two lines the duct's width apart. Once one run is
on the drawing, LIKE is the better way to draw the rest.
