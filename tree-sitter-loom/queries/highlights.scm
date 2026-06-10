"func" @keyword.function
"return" @keyword.return
"if" @keyword.conditional
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
"$" @punctuation.special

(command_name) @function.builtin
(command_arg) @string
(integer) @number
(boolean) @boolean.macro

(type) @type
(comment) @comment @spell

["=" "+" "-" "*" "/" "%" "==" "!=" "<" ">" "<=" ">=" "&&" "||"] @operator
[":" ";" "(" ")" "{" "}"] @punctuation.delimiter
