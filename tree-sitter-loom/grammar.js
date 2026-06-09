/**
 * @file A programming language that compiles to Minecraft datapacks.
 * @author Grady Link <loom@grady.link>
 * @license MIT
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

module.exports = grammar({
  name: "loom",

  extras: ($) => [/[ \t\r]/, $.comment],

  rules: {
    source_file: ($) => repeat(choice($._statement, $._newline)),

    _statement: ($) =>
      seq(
        choice(
          $.variable_declaration,
          $.assignment,
          $.function_definition,
          $.command_statement,
          $.return_statement,
          $.function_call,
        ),
        choice(";", $._newline),
      ),

    _newline: () => /\n/,

    variable_declaration: ($) =>
      seq(
        field("keyword", choice("let", "const")),
        field("name", $.identifier),
        ":",
        field("type", $.type),
        "=",
        field("value", $._expression),
      ),

    assignment: ($) =>
      seq(
        optional("$"),
        field("name", $.identifier),
        "=",
        field("value", $._expression),
      ),

    function_definition: ($) =>
      seq(
        "func",
        field("name", $.identifier),
        "(",
        ")",
        optional(seq(":", field("type", $.type))),
        field("block", $.block),
      ),

    block: ($) => seq("{", repeat(choice($._statement, $._newline)), "}"),

    return_statement: ($) =>
      choice(
        prec(2, seq("return", $._expression)),
        prec(1, "return"),
      ),

    command_statement: ($) =>
      prec(
        -1,
        seq(
          alias($.identifier, $.command_name),
          repeat(choice($.command_arg, $.variable_ref, $.integer)),
        ),
      ),

    _expression: ($) =>
      choice(
        $.binary_expression,
        $.unary_expression,
        $.function_call,
        $.variable_ref,
        $.integer,
        $.parenthesized_expression, // <-- Changed from seq("(", $._expression, ")")
      ),

    parenthesized_expression: ($) => seq("(", $._expression, ")"),

    binary_expression: ($) =>
      choice(
        ...["+", "-", "*", "/", "%"].map((op) =>
          prec.left(
            op === "*" || op === "/" ? 2 : 1,
            seq(
              field("left", $._expression),
              field("operator", op),
              field("right", $._expression),
            ),
          )
        ),
      ),

    unary_expression: ($) =>
      prec(3, seq(field("operator", "-"), field("argument", $._expression))),

    function_call: ($) => seq(field("name", $.identifier), "(", ")"),

    variable_ref: ($) => prec(2, seq("$", field("name", $.identifier))),

    identifier: () => /[a-z_][a-z0-9_]*/i,

    type: () => /[a-z]+/i,

    integer: () => /\d+/,

    command_arg: () => token(prec(-1, /[^\s$;{}()]+/)),

    comment: () => token(seq("--", /.*/)),
  },
});
