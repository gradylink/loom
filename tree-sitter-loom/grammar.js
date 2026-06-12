/**
 * @file A programming language that compiles to Minecraft datapacks.
 * @author Grady Link <loom@grady.link>
 * @license MIT
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

function commaSep(rule) {
  return optional(seq(rule, repeat(seq(",", rule))));
}

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
          $.if,
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

    parameter: ($) =>
      seq(field("name", $.identifier), ":", field("type", $.type)),

    function_definition: ($) =>
      seq(
        "func",
        field("name", $.identifier),
        "(",
        field("parameters", commaSep($.parameter)),
        ")",
        optional(seq(":", field("type", $.type))),
        field("block", $.block),
      ),

    if: ($) =>
      seq(
        "if",
        field("expression", $._expression),
        field("block", $.block),
        optional(seq("else", choice($.if, $.block))),
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
        $.boolean,
        $.parenthesized_expression,
      ),

    parenthesized_expression: ($) => seq("(", $._expression, ")"),

    binary_expression: ($) =>
      choice(
        ...["*", "/", "%"].map((op) =>
          prec.left(
            5,
            seq(
              field("left", $._expression),
              field("operator", op),
              field("right", $._expression),
            ),
          )
        ),
        ...["+", "-"].map((op) =>
          prec.left(
            4,
            seq(
              field("left", $._expression),
              field("operator", op),
              field("right", $._expression),
            ),
          )
        ),
        ...["<", ">", "<=", ">="].map((op) =>
          prec.left(
            3,
            seq(
              field("left", $._expression),
              field("operator", op),
              field("right", $._expression),
            ),
          )
        ),
        ...["==", "!="].map((op) =>
          prec.left(
            2,
            seq(
              field("left", $._expression),
              field("operator", op),
              field("right", $._expression),
            ),
          )
        ),
        prec.left(
          1,
          seq(
            field("left", $._expression),
            field("operator", "&&"),
            field("right", $._expression),
          ),
        ),
        prec.left(
          0,
          seq(
            field("left", $._expression),
            field("operator", "||"),
            field("right", $._expression),
          ),
        ),
      ),

    unary_expression: ($) =>
      prec(
        6,
        seq(
          field("operator", choice("-", "!")),
          field("argument", $._expression),
        ),
      ),

    function_call: ($) =>
      seq(
        field("name", $.identifier),
        "(",
        field("arguments", commaSep($._expression)),
        ")",
      ),

    variable_ref: ($) => prec(2, seq("$", field("name", $.identifier))),

    identifier: () => /[a-z_][a-z0-9_]*/i,

    type: () => /[a-z]+/i,

    integer: () => /\d+/,

    boolean: () => choice("true", "false"),

    command_arg: () => token(prec(-1, /[^\s$;{}()]+/)),

    comment: () => token(seq("--", /.*/)),
  },
});
