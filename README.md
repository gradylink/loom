<h1 valign="center">
<img src="/gradylink/loom/media/branch/main/logo.svg" width="20">
Loom
</h1>

A programming language that compiles to Minecraft datapacks.

## TODO

- [x] Parsing
  - [ ] Parsing Errors
- [ ] Compiling
  - [ ] Improve Internal Errors (e.g. precomputed divide by `0`)
  - [ ] Compiling Errors
    - [ ] Fancy Errors
- [x] Precompute math between literals
- [ ] Generate Datapack
- [ ] Command line stuff
- [ ] Convert commands with variables to equivalent without variables
      (optimizations)
  - [ ] Inside execute run
- [ ] Inline literal const vars
- [ ] Detect variable manipulation (`myVar = $myVar + 1`) and simplify
      operations there.
- [ ] Scope variables
- [ ] Line numbers in expression parsing errors
- [ ] Line numbers in compiler errors
- [ ] Use constants scoreboard to avoid writing constants multiple times
- [ ] Skip early exited code
  - [ ] Unreachable if (e.g. `if true {} else` or `if false`)
  - [ ] Return
  - [ ] Continue
  - [ ] Break
- [ ] Precompute functions used as variables (`return` only thing in function)
- [ ] Remove unused functions

### Implemented Language Features

- [ ] Datapack ID and other meta data
- [ ] Comments
- [ ] Imports
- [ ] Variables
- [ ] Functions
  - [ ] Returns
  - [ ] Arguments
  - [ ] Tags (`#init` on previous line would set init function for example)
  - [ ] Inline Functions
  - [ ] Extern Functions (rn all functions are extern since you can only have 1
        file but this will be important later.)
- [x] Integers
- [ ] Strings
- [ ] Booleans
- [ ] Floats
- [ ] Dictionaries
- [ ] Lists
- [ ] Conditionals
  - [ ] `if`
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
