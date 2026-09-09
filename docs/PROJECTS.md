# Projects

A project is a set of sheets and what stands behind them, described in one
small file beside the drawings. It is never required: a drawing opens the
same with or without one. What it adds is the things the drawings cannot say
for themselves: which files make up the set, in what order, what each is
for, which backgrounds stand behind them, and anything the office wants the
assistant to know about the job.

## Making one

**Tools > Project…** opens the project tool. It starts from the folder the
open drawing is in.

- **Project, Discipline, Sheet template.** The name of the job, the letter
  the sheet numbers start with, and, optionally, the drawing a new sheet is
  made from.
- **Sheets.** One row per sheet: number, title, file, layout, background,
  status. **List drawings in folder** adds every drawing in the folder that
  is not listed yet, reading its number and title from its title block.
  **Add sheet** adds a row with the next number in the pattern of the last.
  An office that keeps several sheets in one file lists them by hand with
  the layout filled in.
- **Backgrounds.** The drawings pulled in behind sheets, usually another
  discipline's plans: a short name and where the file is.
- **Notes for the assistant.** What the drawings cannot say: conventions,
  client rules, what is out of scope.

**Save** writes `project.slate` in the folder. It is plain text, made and
kept by Slate, and can be edited by hand:

```
project: Admin Building HVAC
discipline: M
note: Demolition sheets show existing duct dashed.

# background: name | file
background: A1.1 | ../Arch/A1.1 Floor Plan L1.dwg

# sheet: number | title | file | layout | background | status
sheet: M0.1 | General Notes | M0.1.dwg |  |  |
sheet: M1.1 | Mechanical Plan L1 | M1.1.dwg |  | A1.1 | issued
sheet: M2.1 | Duct Plan L1 | M2.1.dwg |  | A1.1 | draft
```

Files inside the project folder are kept relative to it, so the folder can
move as a whole. Sheets are listed in set order: the numbers are compared
as numbers, so M10.1 follows M9.1.

## What it does

When a drawing is opened, the folder it is in is checked for a project
file. If one is there, the set is known, and the history says so. A project
opened by hand stays open when a drawing from elsewhere is opened.

### The Project panel

**View > Panels > Project** shows the set: the sheets in order, the open one
in bold, and each one's state: its status from the list, whether its file
is missing, and whether the background behind it has changed since the
sheet was last saved. Double-click a sheet, or select it and press **Open
sheet**, to open it. **Project…** opens the tool. The panel comes up on
its own the first time a project is found.

Each sheet is read the way DESCRIBE reads the open drawing, once, with its
backgrounds followed, and the reading kept in `project.slate.index` beside
the project until the sheet's file changes. Reading a set of twenty sheets
takes a few seconds the first time and none after that.

The assistant is told the set with every request: one line per sheet with
its number, title, background, status, and a gist of what it holds, plus
which sheet the open drawing is. It can ask for more:

```
ASK SHEET M2.1     what that sheet holds, in full
ASK PROJECT        every sheet and its backgrounds
```

That is what makes questions across sheets possible: which sheets show the
mechanical room, whether every plan sheet has the current background, what
else refers to a piece of equipment that moved.

## Scripted runs

`--project <file>` uses a project file for the session, whatever sits
beside the drawing. With `--agent-context` it prints what the assistant
would be told, project included.
