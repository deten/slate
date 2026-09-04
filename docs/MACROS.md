# Custom commands and macros

A macro is **the command line, written down**. Every step in a macro file is
text that would have worked had you typed it, and it is turned into input by the
same code that reads what you type. There is one command language; a macro does
not get a second one.

A macro file becomes a real command: it appears on the menus, takes an alias,
accepts a keyboard shortcut, is reachable from a script, and can be called by
another macro. Nothing that consumes commands knows or cares that it came from a
file.

## Where the files go

    <install folder>/commands/          shipped with Slate — read-only
    <your config folder>/commands/      yours

On Windows the second is usually

    C:\Users\<you>\AppData\Local\Slate\commands\

Files end in `.macro`. Both folders are read at start-up and whenever a drawing
is opened or created, so editing a file and pressing Ctrl+N is enough to try a
change.

**A file in your folder shadows a shipped file of the same name.** That is how
you customise a supplied command: copy it across and edit the copy. Editing the
shipped one works until the next update replaces it, and then the edit is gone
with no way back.

## A file

```
# A leader carrying a standard note.

name: WALLTAG
alias: WT
title: Wall Tag
category: Annotate
help: Leader with the standard stud-wall note
param: SIZE = 2x6 | Stud size

LEADER
\
\
\
;
TYP. $SIZE STUD WALL
```

A header of `key: value` lines, a blank line, then the body. Comments start with
`#` and may appear anywhere, including above the header.

### Header

| Key | Meaning |
|---|---|
| `name` | What you type to run it. Required. No spaces. |
| `alias` | Alternative names, comma separated. |
| `title` | Short label for menus. Defaults to the name. |
| `help` | One line describing it. |
| `category` | Which menu and palette group it files under. Defaults to `Custom`. |
| `param` | A value to ask for. See below. |

### Body

| Line | Means |
|---|---|
| anything | Typed at the command line: a command name, a coordinate, a keyword, a word of text |
| `;` | Enter — accept a default, or end an open-ended command like LINE |
| `\` | **Pause**: hand the next prompt to the user, then carry on |
| `# …` | A comment |
| *(blank)* | Nothing. Blank lines are free, so a macro can be grouped to read well |

To type a literal `;`, `\` or `#` at the start of a line, escape it: `\;`, `\\`,
`\#`.

> **A blank line is not Enter.** This is where other CAD software's script files trip
> people: a file laid out to read clearly does something different from the same
> file written densely. Here Enter is spelled `;` and blank lines mean nothing.

### Parameters

    param: SIZE = 2x6 | Stud size

`SIZE` is the name, `2x6` is the default, `Stud size` is the prompt. Everything
after the name is optional. The macro asks for each parameter before it runs;
pressing Enter takes the default.

Use `$SIZE` anywhere in the body. An unknown `$NAME` is left alone, so `$50` at a
text prompt is still fifty dollars.

Because parameters are ordinary prompts, they are also how a macro is driven
from a script or the test harness — the value is just the next input.

## Pausing is the point

A macro that cannot stop is a recording. Almost every useful macro has the shape
*run a command, let the drafter place it, then supply the rest*:

```
name: SQUARE
param: SIDE = 100 | Side length

RECTANGLE
\
@$SIDE,$SIDE
```

`RECTANGLE` starts, `\` hands over the first corner, and the second corner is
computed from wherever they picked — `@` is a relative coordinate, and it works
here exactly as it works when typed.

**Escape stops the whole macro**, not just the command it was inside.

## When a macro goes wrong

A step the running command will not accept stops the macro there and says so,
naming the line:

    Macro stopped at line 6: "sideways" is not what the command is asking for.

The rest is abandoned rather than fed to whatever runs next — a stray coordinate
becoming a command nobody asked for is worse than stopping.

A file that will not parse costs you that file and no others; the reason is
written to the command history at start-up, naming the file and the line. A
macro whose name is already taken by a built-in is refused and reported, rather
than silently replacing it.

A macro that runs itself stops after ten thousand steps and says so.

## SCRIPT

`SCRIPT` runs a file of steps once without registering anything — the body
format above, with no header:

```
RECTANGLE
0,0
40,30
```

The difference is *do this now* versus *give me a command that does this*.

## What a macro cannot do

There are no conditionals, no arithmetic and no access to the drawing's
contents. A macro can only do what you could have done by hand, which is also
what makes it safe to run one: there is no second path into the database.

For anything more, the plugin API (`docs/PLUGINS.md`) exposes documents,
entities, layers, blocks, geometry, selection and transactions to a native
library, and registers commands through the same mechanism a macro uses.
