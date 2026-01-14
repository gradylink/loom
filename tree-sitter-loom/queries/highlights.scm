"func" @keyword.function
"return" @keyword.return
["let" "const"] @keyword.storage

(function_definition 
  name: (identifier) @function)

(function_call 
  name: (identifier) @function.call)

(variable_declaration 
  name: (identifier) @variable)

(assignment 
  name: (identifier) @variable)

(variable_ref) @variable.parameter
"$" @punctuation.special

(command_name) @function.builtin

(command_arg) @string

(integer) @number
(type) @type
(comment) @comment @spell

["=" "+" "-" "*" "/" "%"] @operator
[":" ";" "(" ")" "{" "}"] @punctuation.delimiter
