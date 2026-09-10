# References: other drawings, PDFs and pictures

A drawing can show files it does not contain: another drawing, a page of a
PDF, or a picture. The file stays where it is; the drawing remembers its path
and where to put it.

## Commands

| Command | What it does |
| --- | --- |
| `XATTACH` (XA) | Attach another DWG or DXF as an external reference. Asks for the file, whether it is an attachment or an overlay, then the insertion point, scale and rotation. The reference is placed like a block and shows the other drawing's contents. An overlay is seen here only and not through further references. |
| `PDFATTACH` (PDFA) | Place a page of a PDF under the drawing to trace over. Asks for the file, the page when there is more than one, then the insertion point, scale and rotation. The page is placed at its printed size, one PDF inch to the drawing's inch, times the scale. |
| `IMAGEATTACH` (IAT) | Place a picture file. Asks for the file, then the insertion point, scale and rotation. One pixel is one drawing unit at scale 1. |
| `REFERENCES` (XREF, IMAGE) | The reference manager: every picture and PDF the drawing depends on, whether it was found, Locate to repair a path, Reload to read the files again, and Detach to remove one with every place it is shown. Detach undoes as one step. |
| `DRAWORDER` (DR) | Which objects draw over which: to the front, to the back, or just above or under another object. A PDF page or a picture sent to the back stops hiding the lines drawn over it. |
| `XCLIP` (XC) | Show only part of a block or external reference. Select it, then New for a boundary, Rectangular by two corners or Polygonal by points until Enter; OFF shows the whole again keeping the boundary, ON brings it back, Delete drops it. The boundary moves with the reference. |
| `IMAGECLIP` (ICL) | The same for a picture: New, ON, OFF, Delete. The boundary is kept in the picture's own pixels, so it stays put when the picture is moved or scaled. |

## What is drawn

A PDF page is rendered from the file and drawn in place, so it can be traced
over. A picture is drawn from its file. A referenced drawing shows its
contents. When a file cannot be found the drawing shows a frame with the path
it is looking for, and the reference manager can point it at the file.

## In the file

A reference is saved the way other CAD software saves it: the block record
with the path for a drawing, an image definition for a picture, a PDF
definition naming the page for a PDF, each listed in the drawing's named
object dictionary, with the classes those objects need declared. A file saved
here opens elsewhere with its references intact.

A clipped reference keeps its boundary as a spatial filter in the
reference's extension dictionary, which is how every other reader expects
it, so a clip made here is honoured elsewhere and one made elsewhere is
drawn here. A clipped picture keeps its boundary in the image record.

## Not yet

External drawings in the manager, with detach, reload and unload for them.
Attaching DWF and DGN underlays.
