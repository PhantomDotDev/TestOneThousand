# Material Scripting System

Add and edit materials (like Sand, Water, Lava, Glass...) for the falling-sand
engine **without writing any C++ and without recompiling the game.** You write
a small text file, click one button in-game, and your material is alive.

This document assumes you've never written code before. Every term is
explained the first time it shows up. If you get stuck, skip to
[Troubleshooting](#troubleshooting) — most beginner problems are listed there
with the exact fix.

---

## Table of Contents

1. [What is this?](#1-what-is-this)
2. [Opening the Material Editor in-game](#2-opening-the-material-editor-in-game)
3. [Your first material, step by step](#3-your-first-material-step-by-step)
4. [The `.mat` file format — full reference](#4-the-mat-file-format--full-reference)
5. [Making materials react to each other](#5-making-materials-react-to-each-other)
6. [Using your material in the game](#6-using-your-material-in-the-game)
7. [Example materials you can copy-paste](#7-example-materials-you-can-copy-paste)
8. [Troubleshooting](#8-troubleshooting)
9. [For programmers: how it works under the hood](#9-for-programmers-how-it-works-under-the-hood)

---

## 1. What is this?

Normally, adding a new material to a game like this (say, "Honey" or
"Acid") means a programmer has to open the engine's source code, write C++,
and rebuild the whole game. That's slow, and it means only programmers can
add new content.

This system lets **anyone** create a new material by writing a small,
simple text file — no programming language to install, no compiler, nothing
to download. You type out some plain rules like:

```
material Acid {
    color = 0xFF88FF00
    move_down = true
}
```

...save the file, press **Compile + Apply** inside the game, and "Acid" now
exists as a real, paintable material — falling, colliding, reacting — all
without touching a single line of the game's actual programming.

There are two kinds of materials in this engine:

- **Built-in materials** (Sand, Water, Rock, Lava) — these are written by the
  programmer in real C++ for maximum performance. You can't edit these
  directly, but your custom materials can still react with them (e.g. "when
  my material touches Water, turn into Rock").
- **Your custom materials** — written by you, as plain text files ending in
  `.mat`. These are what this guide teaches you to make.

---

## 2. Opening the Material Editor in-game

1. Launch the game as normal.
2. Look for the small window titled **"Main Menu"** (usually top-left).
3. Click the button **"Materials Editor"**.

A new window titled **Material Editor** will open. It has three parts:

```
┌───────────────┬─────────────────────────────┬───────────────────┐
│  Materials/    │                             │ Registered        │
│  (file list)   │      Text editor            │ Materials         │
│                │      (where you type)       │ (list of what's   │
│  Glass.mat     │                             │  currently active)│
│  Acid.mat      │                             │                   │
│                │                             │                   │
└───────────────┴─────────────────────────────┴───────────────────┘
```

- **Left panel** — every `.mat` file you've saved. Click one to open it.
- **Middle** — the actual text editor, with color-coded syntax (keywords,
  numbers, etc. show in different colors to help you read it).
- **Right panel** — every material currently active in the game right now,
  with a little tag showing `(native)` for built-in ones or `(script)` for
  yours.

At the top of the window are the buttons you'll use constantly:

| Button | What it does |
|---|---|
| **New** | Start a brand new material from a template |
| **Save** | Save your current file to disk |
| **Save As...** | Save under a new name |
| **Reload From Disk** | Throw away unsaved changes, reload the saved version |
| **Compile + Apply** | Check your file for mistakes and, if there are none, make it live in the game *immediately* |

---

## 3. Your first material, step by step

Let's make a material called **Honey** — thick, sticky, golden, and slow to
fall.

### Step 1 — Click "New"

A small popup appears asking for a name. Type:

```
Honey
```

Click **Create**. The editor now shows a starter template that looks like
this:

```
material Honey {
    color = 0xFFFFFFFF
    move_down = true
    move_down_side = true
    move_side = false
    density = 1

    on_touch(WATER) {
        if chance(10) {
            swap
        }
    }
}
```

Don't worry about understanding every line yet — we'll go through it.

### Step 2 — Change the color

Find this line:

```
color = 0xFFFFFFFF
```

This is currently plain white. Colors are written as `0xAARRGGBB` — a
special code made of 8 letters/numbers after `0x`:

- `AA` = **A**lpha (how see-through it is — `FF` means fully solid)
- `RR` = how much **R**ed
- `GG` = how much **G**reen
- `BB` = how much **B**lue

Each pair goes from `00` (none) to `FF` (max). A honey-gold color is mostly
red and green, no blue:

```
color = 0xFFE6A817
```

You don't need to calculate this by hand — you can look up "hex color
picker" online, pick a color visually, and it'll usually give you 6
characters like `E6A817`; just put `0xFF` in front of it.

### Step 3 — Decide how it moves

These four lines control movement:

```
move_down = true
move_down_side = true
move_side = false
```

- `move_down` — falls straight down when there's empty space below (like sand)
- `move_down_side` — falls diagonally when blocked straight down (like sand
  piling into a triangle)
- `move_side` — spreads out sideways even when not falling (like water
  flooding flat) — leave this `false` for honey, since honey should be
  thick and NOT flow sideways easily

Leave these as they are for Honey — they already do what we want.

### Step 4 — Save it

Click **Save**. If this is a new material, it will save to
`Materials/Honey.mat`.

### Step 5 — Compile it

Click **Compile + Apply**.

- If everything is correct, you'll see a **green message** at the top:
  `Compiled OK - material registered/updated live.`
- If you made a typo, you'll see a **red message**, and a red squiggly
  line will appear under the exact spot in your text that's wrong (just
  like a spell-checker). See [Troubleshooting](#8-troubleshooting).

### Step 6 — Paint with it in-game

1. Close (or leave open) the Material Editor.
2. In the **Main Menu** window, click **"Pick Custom Material"**.
3. A list of every material appears, each shown as a colored button. Click
   **Honey**.
4. Go to the **Brushes** window and click the **Custom** swatch (the last
   colored button in the row).
5. Left-click and drag in the game world — you're now painting Honey!

That's it. You made a working material with zero programming experience.

---

## 4. The `.mat` file format — full reference

Every `.mat` file follows this shape:

```
material <Name> {
    <fields...>
    <reactions...>
}
```

### 4.1 Naming rules

- The material name (right after the word `material`) can use letters and
  numbers, no spaces. `Honey`, `Acid2`, `StickyMud` are all fine.
  `Sticky Mud` (with a space) is **not** allowed.
- Everything about your material must be wrapped in curly braces `{ }`,
  matching exactly one open `{` to one close `}`.

### 4.2 Fields (the material's basic properties)

Write these as `field_name = value`, one per line. Order doesn't matter.

| Field | Type | Meaning |
|---|---|---|
| `color` | color code, e.g. `0xFFAABBCC` | The material's display color |
| `move_down` | `true` / `false` | Falls straight down into empty space |
| `move_down_side` | `true` / `false` | Falls diagonally when blocked below |
| `move_side` | `true` / `false` | Spreads sideways (liquids) |
| `move_up` | `true` / `false` | Rises upward (smoke, gas, fire) |
| `move_up_side` | `true` / `false` | Rises diagonally |
| `density` | a number, e.g. `1`, `3` | Higher = heavier. Affects who sinks past who. |

**Important:** if you don't set *any* of the `move_*` fields to `true`, your
material is treated as **solid/static** — it behaves like Rock and never
moves, no matter what's beneath it. This is what you want for things like
Glass, Wood, or Metal.

You can also invent your **own** custom fields with any name, as long as
the value is a number or `true`/`false`:

```
viscosity = 4
is_flammable = true
```

These don't do anything on their own, but you can read them back inside
your reactions using `chance(viscosity)` etc. (advanced — see
Section 9 if you're curious).

### 4.3 Comments

Anything after `//` on a line is ignored — use it to leave yourself notes:

```
color = 0xFFE6A817   // honey gold
```

You can also write multi-line notes:

```
/*
   This is a note that can span
   several lines.
*/
```

### 4.4 Reactions — `on_touch`

This is how your material responds to bumping into another material. The
shape is:

```
on_touch(OTHER_MATERIAL_NAME) {
    <what to do>
}
```

`OTHER_MATERIAL_NAME` must be a material that already exists — either a
built-in one (`SAND`, `WATER`, `ROCK`, `LAVA` — always type these in ALL
CAPS) or another one of your scripted materials (using whatever name you
gave it, e.g. `Honey`, `Glass`).

Example — Honey touching Fire catches fire and becomes Ash:

```
on_touch(LAVA) {
    become Ash
}
```

You can have as many `on_touch(...)` blocks as you like, one for each
material you care about reacting to.

### 4.5 What you can put inside a reaction

These are the actions available inside `on_touch` (and `on_update`, covered
next):

| Command | Effect |
|---|---|
| `swap` | Trade places with the material you touched |
| `become NAME` | Turn yourself into a different material |
| `destroy` | Remove yourself (turn into empty space) |
| `spawn(NAME)` | Create a new particle of material `NAME` nearby |
| `set_color(0xAARRGGBB)` | Change your own color right now |
| `if <condition> { ... }` | Only do the following if the condition is true |
| `if <condition> { ... } else { ... }` | Do one thing or another |
| `chance(N)` | A condition that's true `N` percent of the time |

### 4.6 Using `chance()` for randomness

Real materials aren't 100% predictable — water doesn't *always* let sand
sink through it instantly. `chance(N)` gives you a random yes/no, true `N`
times out of 100:

```
on_touch(WATER) {
    if chance(15) {
        swap
    }
}
```

This means: every time this material touches water, there's a 15% chance
(roughly 1 in 7) that it swaps places that frame. Since this check runs
repeatedly while they're touching, it'll still happen "eventually" even at
a low percentage — low numbers = slower/rarer, high numbers = faster/more
often.

- `chance(100)` = always happens
- `chance(0)` = never happens
- `chance(50)` = coin flip

### 4.7 `if` / `else`

```
on_touch(WATER) {
    if chance(50) {
        swap
    } else {
        set_color(0xFF3355AA)
    }
}
```

This says: 50% of the time, swap places with the water. The other 50% of
the time, just change color instead (as if getting wet/darker).

You can leave off the `else` entirely if you don't need a "otherwise" case:

```
if chance(20) {
    destroy
}
```

### 4.8 `on_update` — reactions with no trigger needed

If you want something to happen constantly, not just when touching a
specific material, use `on_update` instead of `on_touch(...)`:

```
on_update() {
    if chance(1) {
        set_color(0xFFFF8800)
    }
}
```

This runs continuously (checked periodically), useful for slow effects like
"randomly flicker" or "slowly decay."

---

## 5. Making materials react to each other

Here's the full mental model:

- Every occupied cell in the world checks its four neighbors (up, down,
  left, right) periodically.
- If your material has an `on_touch(X)` block and one of those neighbors is
  material `X`, your block runs.
- Whatever your block decides (swap, become, destroy, etc.) actually happens
  in the game world immediately after.

**Built-in material names you can react to** (always type these in capital
letters exactly like this):

- `SAND`
- `WATER`
- `ROCK`
- `LAVA`

**Your own scripted materials** — just use whatever name you gave them when
you typed `material NameHere { ... }`. Reactions work both directions and
across script files — e.g. `Honey` can react to `Acid` and vice versa, as
long as both `.mat` files have been compiled at least once.

---

## 6. Using your material in the game

Once a material is compiled successfully, you have two ways to actually
place it into the world:

### Option A — Custom Brush (easiest, no extra setup)

1. Main Menu → **Pick Custom Material** → click your material's name.
2. Brushes window → click the **Custom** swatch (last button, colored to
   match your material).
3. Paint by left-click-dragging in the world.

### Option B — Reactions happen automatically

You don't need to do anything extra for reactions — if your material has an
`on_touch(WATER)` block, it'll trigger automatically any time it's placed
next to water, whether you painted it there yourself or it was created by
another reaction (like `spawn(...)`).

---

## 7. Example materials you can copy-paste

Copy any of these into a new material file to see them work, then tweak
values to make them your own.

### Glass — solid, made when Lava touches Water

```
material Glass {
    color = 0xFFAADDFF
    density = 2
    // No move_* fields set -> this material is solid/static, like Rock.
}
```

### Steam — rises up, occasionally appears from Lava + Water

```
material Steam {
    color = 0x88DDDDDD
    move_up = true
    move_up_side = true
}
```

### Acid — eats through Sand

```
material Acid {
    color = 0xFF88FF00
    move_down = true
    move_down_side = true
    move_side = true

    on_touch(SAND) {
        if chance(5) {
            destroy
        }
    }
}
```

### Ash — leftover after something burns

```
material Ash {
    color = 0xFF555550
    move_down = true
    move_down_side = true
    density = 1
}
```

### Slime — bounces between two colors and slowly spreads

```
material Slime {
    color = 0xFF66DD66
    move_down = true
    move_side = true
    density = 1

    on_update() {
        if chance(2) {
            set_color(0xFF44BB44)
        }
    }
}
```

---

## 8. Troubleshooting

### "Compile failed" and I don't understand the error

Look at the **red message** at the top of the editor and the **line
number** it mentions — the text editor also underlines the problem spot in
red. Common causes below, listed by the error message you'll actually see:

**`Expected 'material' keyword at start of file`**
Your file doesn't start with the word `material`. Make sure the very
first word in the file (ignoring comments) is literally `material`.

**`Expected '}'`**
You're missing a closing curly brace somewhere. Every `{` needs a matching
`}`. Count them — if you have 3 opening braces, you need exactly 3 closing
ones.

**`Expected color literal (e.g. 0xFFAABBCC)`**
Your `color = ...` line has something that isn't a valid color code. Make
sure it starts with `0x` and has exactly 8 characters after that (letters
A-F and numbers 0-9 only).

**`Expected true/false for field 'move_down'`**
You wrote something other than `true` or `false` for a movement field —
check for typos like `ture` or `treu`.

**`Unknown field 'xyz' with unsupported value`**
You tried to set a custom field to something that isn't a plain number or
`true`/`false`. Custom fields can only hold simple values like `4` or
`true`, not text or colors.

**`Expected material name after 'become'`**
You wrote `become` but didn't follow it with a valid material name right
after, e.g. `become` with nothing after it, or `become "Rock"` (don't use
quote marks).

### My material compiled fine but painting it doesn't do anything

- Did you click **Pick Custom Material** and actually select it? The
  Custom brush always paints whichever material you last picked there.
- Did you click the **Custom** swatch in the Brushes panel (not Sand/Water/
  etc.)? Painting with a different brush selected will paint that brush's
  material instead.

### My material's reaction never seems to trigger

- Double check the material name inside `on_touch(...)` is spelled *exactly*
  right, including capitalization. Built-in materials are always ALL CAPS
  (`WATER`, not `Water`). Your own materials must match the name you used
  after the word `material` at the top of that file.
- Remember `chance(N)` is a percentage *per check*, not guaranteed — a low
  number like `chance(2)` can take a while to actually trigger. Try
  `chance(100)` temporarily to confirm the reaction logic itself works,
  then dial the percentage back down once you've confirmed it.
- Make sure you actually clicked **Compile + Apply** after your last edit —
  saving the file alone does not make changes live in the running game.

### I edited a file outside the game (e.g. in Notepad) and it's not showing up

Click **Reload From Disk** in the Material Editor, then **Compile + Apply**
again. The game only reads the file fresh when you explicitly reload or
re-open it.

### I accidentally broke a material that used to work

Every unsuccessful compile leaves the *previous working version* still
running in the game — a bad edit never deletes a good material. Click
**Reload From Disk** to throw away your broken edit and get back the last
saved version, or just keep fixing the red-underlined error and try
**Compile + Apply** again.

---

## 9. For programmers: how it works under the hood

If you do write code, here's the short version of the architecture, so you
know where to look:

- **`MaterialScript.h/.cpp`** — a hand-written tokenizer + recursive-descent
  parser + tree-walking interpreter for the `.mat` DSL. No external
  dependencies, no real C++ compilation involved — this is a small,
  purpose-built language, not a C++ subset.
- **`MaterialRegistry.h/.cpp`** — a single registry that holds both
  "native" materials (registered from engine C++ via
  `RegisterHardcodedMaterial` + `SetNativeOnTouch`/`SetNativeOnUpdate`
  callbacks) and "scripted" materials (parsed `.mat` files), addressed
  uniformly by `MaterialId`. Simulation code calls `RunOnTouch`/
  `RunOnUpdate` without needing to know which kind it's talking to.
- **`MaterialEditor.h/.cpp`** — the in-game ImGui window, built on top of
  the ImTextEdit `TextEditor` widget, with a custom `LanguageDefinition`
  for syntax highlighting and `SetErrorMarkers` wired directly to parser
  error locations.
- **`main.cpp` integration** — `RegisterEngineMaterials()` bridges the
  engine's compile-time `ParticleType` enum (`SAND`/`WATER`/`ROCK`/`LAVA`)
  to registry `MaterialId`s by name, and `RunMaterialReactionsAt()` runs an
  additive reaction pass over active/awake chunks after the core CA step,
  applying whatever outcome (`swap`/`become`/`destroy`/`set_color`/`spawn`)
  a handler requests directly to the `World`.

Extending it further (e.g. giving scripted materials their own distinct
movement behavior instead of borrowing SAND/ROCK's shape) means adding a
real `MaterialId` field to `Particle` itself and updating `SandWorld.h`'s
CA to consult the registry per-cell rather than switching on
`ParticleType` — a bigger but very achievable next step.
