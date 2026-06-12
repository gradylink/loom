"func" @keyword.function
"return" @keyword.return
"if" @keyword.conditional
"else" @keyword.conditional
["let" "const"] @keyword.storage

(function_definition name: (identifier) @function)
(function_call name: (identifier) @function.call)
(variable_declaration name: (identifier) @variable)
(parameter name: (identifier) @variable.parameter)

(assignment name: (identifier) @variable)
(variable_ref "$" @punctuation.special name: (identifier) @variable.reference)

(command_name) @function.builtin
(command_arg) @string

(type) @type
(integer) @number
(boolean) @boolean

(unary_expression operator: _ @operator)
(binary_expression operator: _ @operator)
[":" ";" "(" ")" "{" "}" ","] @punctuation.delimiter

(comment) @comment @spell
