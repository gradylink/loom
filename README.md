<h1 valign="center">
<img src="/gradylink/loom/media/branch/main/logo.svg" width="20">
Loom
</h1>

A programming language that compiles to Minecraft datapacks.

## TODO

- [x] Parsing
  - [ ] Parsing Errors
- [x] ] Compiling
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
- [ ] Detect variable manipulation (`myVar = $myVar + 1`) and simplify
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
  - [ ] Arguments
  - [ ] Tags (`#init` on previous line would set init function for example)
  - [ ] Inline Functions
  - [ ] Extern Functions (rn all functions are extern since you can only have 1
        file but this will be important later.)
- [x] Integers
- [ ] Strings
- [x] Booleans
- [ ] Floats
- [ ] Dictionaries
- [ ] Lists
- [ ] Conditionals
  - [x] `if`
  - [ ] `else`
  - [ ] `elif`
- [ ] Loops
  - [ ] `while`
  - [ ] `for`
  - [ ] `foreach`
- [ ] Inline datapack features
  - [ ] Recipes
  - [ ] Enchantments
  - [ ] Advancements
  - [ ] Dialogs
  - [ ] Tags
  - [ ] Loot Tables
