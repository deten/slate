# Attributes

An attribute is tagged text a block carries and each reference fills in: the
room name on a room tag, the sheet number in a title block, the size on a
door symbol. The definition lives in the block; the value lives on the
reference.

## Commands

| Command | What it does |
|---|---|
| `ATTDEF` (ATT) | Makes an attribute definition in the current space: modes, tag, prompt, default, then where the text goes, its height and angle. BLOCK sweeps it into a definition like any other entity. |
| `INSERT` | When the block has attribute definitions, asks for each value (the definition's prompt, with its default). Constant attributes are never asked; preset ones take their default. |
| `ATTEDIT` (ATE, EATTEDIT) | Picks a block reference, or takes the one selected, and asks a new value for each of its attributes, current value as the default. |

Modes: *Invisible* attributes are stored but not drawn; *Constant* ones have
one value for every reference and are drawn from the definition; *Verify*
and *Preset* are carried for the file.

## On screen and in the file

A definition in the open shows its tag; seen through a reference, only the
reference's own values appear. A reference and its attributes move, copy,
rotate and erase as one: selecting the reference selects them.

In the file, attributes follow their INSERT as ATTRIB records closed by
SEQEND, the standard form. Copies of a reference get copies of its
attributes, linked to the copy.
