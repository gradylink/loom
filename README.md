<h1 valign="center">
<img src="/gradylink/loom/media/branch/main/logo.svg" width="20">
Loom
</h1>

A programming language that compiles to Minecraft datapacks.

## TODO

- [x] Parsing
  - [ ] Parsing Errors
- [ ] Compiling
  - [ ] Compiling Errors
- [ ] Precompute math between literals
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

### Implemented Language Features

- [ ] Datapack ID and other meta data
- [ ] Comments
- [ ] Imports
- [ ] Variables
- [ ] Functions
  - [ ] Returns
  - [ ] Arguments
  - [ ] Inline Functions
  - [ ] Extern Functions (rn all functions are extern since you can only have 1
        file but this will be important later.)
- [ ] Integers
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
