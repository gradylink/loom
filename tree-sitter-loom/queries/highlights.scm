"func" @keyword.function
"return" @keyword.return
"import" @keyword.import
["export" "extern"] @keyword.storage
["let" "const" "enum" "struct"] @keyword.storage
["if" "else"] @keyword.conditional
["while" "do" "for"] @keyword.repeat
(for "in" @keyword.repeat)

["as" "at" "align" "anchored" "facing" "on" "positioned" "rotated"] @keyword.conditional
(contextModifier "in" @keyword.conditional)
("facing" "entity" @keyword.conditional)
("positioned" [ "as" "over" ] @keyword.conditional)
("rotated" "as" @keyword.conditional)

["eyes" "feet"] @constant.builtin
["attacker" "controller" "leasher" "origin" "owner" "passengers" "target" "vehicle"] @constant.builtin
["world_surface" "motion_blocking" "motion_blocking_no_leaves" "ocean_floor"] @constant.builtin

"#" @attribute
(function_definition tag: (namespaced_arg) @attribute)
(contextModifier dim: (namespaced_arg) @constant)

(selector [ "@s" "@r" "@p" "@e" "@a" "@n" ]) @variable.builtin
(selector) @variable.parameter

(struct_definition name: (identifier) @type)
(struct_field name: (identifier) @property)
(enum_definition name: (identifier) @type)
(enum_variant name: (identifier) @constant)

(property_access property: (identifier) @property)
(member_expression property: (identifier) @property)

(member_expression
  object: (variable_ref name: (identifier) @type)
  property: (identifier) @constant
  (#match? @type "^[A-Z]")
  (#match? @constant "^[A-Z]"))

(function_definition name: (identifier) @function)
(function_call name: (identifier) @function.call)
(variable_declaration name: (identifier) @variable)
(parameter name: (identifier) @variable.parameter)
(assignment name: (identifier) @variable)
(for iterator: (identifier) @variable)
(variable_ref name: (identifier) @variable.reference)

(command_name) @function.builtin
(command_arg) @string
(string_literal) @string

(type (identifier) @type)
(swizzle) @type
(integer) @number
(float) @number
(boolean) @boolean
(vec2) @number
(vec3) @number

".." @operator
"=" @operator
(unary_expression operator: _ @operator)
(binary_expression operator: _ @operator)
(struct_expression name: (identifier) @type)
(struct_expression_field name: (identifier) @property)
[":" ";" "(" ")" "{" "}" "," "[" "]" "[]" "!" "."] @punctuation.delimiter

(comment) @comment @spell

(interpolation ["${" "}"] @punctuation.special)

(function_call name: (identifier) @function.builtin (#match? @function.builtin "^(append|remove|insert|len)$"))
