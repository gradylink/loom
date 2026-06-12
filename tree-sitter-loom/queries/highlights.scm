"func" @keyword.function
"return" @keyword.return
"if" @keyword.conditional
"else" @keyword.conditional
["let" "const"] @keyword.storage

(function_definition 
  name: (identifier) @function)

(function_call 
  name: (identifier) @function.call)

(variable_declaration 
  name: (identifier) @variable)

(assignment 
  name: (identifier) @variable)

(variable_ref) @variable.reference

(parameter
  name: (identifier) @variable.parameter)

(type) @type
(command_name) @function.builtin
(command_arg) @string

(integer) @number
(boolean) @boolean

"$" @punctuation.special
["=" "+" "-" "*" "/" "%" "==" "!=" "<" ">" "<=" ">=" "&&" "||"] @operator
[":" ";" "(" ")" "{" "}"] @punctuation.delimiter

(comment) @comment @spell
