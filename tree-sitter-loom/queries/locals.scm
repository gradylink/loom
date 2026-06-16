(block) @local.scope
(function_definition) @local.scope

(enum_definition
  name: (identifier) @local.definition.enum)

(enum_variant
  name: (identifier) @local.definition.constant)

(parameter
  name: (identifier) @local.definition.parameter)

(variable_declaration
  name: (identifier) @local.definition.variable)

(variable_ref
  name: (identifier) @local.reference)

(member_expression
  object: (identifier) @local.reference)

(function_definition
  name: (identifier) @local.definition.function)

(function_call
  name: (identifier) @local.reference)
