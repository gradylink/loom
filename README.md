<h1 valign="center">
<img src="logo.svg" width="25">
Loom
</h1>

A programming language that compiles to Minecraft datapacks.

## TODO

- [x] Parsing
  - [ ] Parsing Errors
- [x] Compiling
  - [x] Improve Internal Errors (e.g. precomputed divide by `0`)
  - [x] Compiling Errors
    - [x] Fancy Errors (well kind of)
- [x] Precompute math between literals
- [x] Generate Datapack
- [x] Command line stuff
- [x] Convert commands with variables to equivalent without variables
      (optimizations)
  - [ ] Inside execute run
- [ ] Inline literal const vars
- [x] Detect variable manipulation (`myVar = $myVar + 1`) and simplify
      operations there.
- [x] Scope variables
- [ ] Line numbers in expression parsing errors
- [x] Line numbers in compiler errors
- [x] Skip early exited code
  - [x] Unreachable if (e.g. `if true {} else` or `if false`)
  - [x] Return
  - [ ] Continue (not added yet)
  - [ ] Break (not added yet)
- [ ] Precompute functions used as variables (`return` only thing in function)
- [ ] Remove unused functions

### Implemented Language Features

- [ ] Datapack ID and other meta data
- [x] Comments
- [ ] Imports
- [x] Variables
- [x] Functions
  - [x] Returns
  - [x] Arguments
  - [x] Tags (`#init` on previous line would set init function for example)
  - [ ] Inline Functions (Macros?)
  - [ ] Extern Functions (rn all functions are extern since you can only have 1
        file but this will be important later.)
- [x] Integers
- [ ] Strings
- [x] Booleans
- [ ] Floats
- [ ] Dictionaries
- [ ] Lists
- [ ] Enums
- [ ] Structs (more optimized than dictionaries)
- [x] Conditionals
  - [x] `if`
  - [x] `else`
  - [x] `else if`
- [ ] Loops
  - [x] `while`
  - [x] `do while`
  - [x] `for`
  - [ ] `foreach` (no lists yet)
- [ ] Inline datapack features
  - [ ] Recipes
  - [ ] Enchantments
  - [ ] Advancements
  - [ ] Dialogs
  - [ ] Tags
  - [ ] Loot Tables
