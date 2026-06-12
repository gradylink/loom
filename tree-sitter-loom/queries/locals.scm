(block) @local.scope
(function_definition) @local.scope

(parameter
  name: (identifier) @local.definition.parameter)

(variable_declaration
  name: (identifier) @local.definition.variable)

(variable_ref
  name: (identifier) @local.reference)

(function_definition
  name: (identifier) @local.definition.function)

(function_call
  name: (identifier) @local.reference)
