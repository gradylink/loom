/**
 * @file A programming language that compiles to Minecraft datapacks.
 * @author Grady Link <loom@grady.link>
 * @license MIT
 */

/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

/**
 * @param {RuleOrLiteral} rule
 */
function commaSep(rule) {
  return optional(seq(rule, repeat(seq(",", rule))));
}

/**
 * @param {RuleOrLiteral} rule
 */
function multilineCommaSep(rule) {
  return optional(seq(rule, repeat(seq(",", optional(/\n/), rule))));
}

module.exports = grammar({
  name: "loom",

  extras: ($) => [/[ \t\r]/, $.comment],

  conflicts: ($) => [
    [$.selector],
  ],

  rules: {
    source_file: ($) => repeat(choice($._statement, $._newline)),

    _statement: ($) =>
      seq(
        choice(
          $.import_statement,
          $.enum_definition,
          $.variable_declaration,
          $.assignment,
          $.function_definition,
          $.if,
          $.while,
          $.do_while,
          $.for,
          $.command_statement,
          $.return_statement,
          $.function_call,
          $.context_statement,
        ),
        choice(";", $._newline),
      ),

    _newline: () => /\n/,

    import_statement: ($) => seq("import", field("path", $.path)),

    path: () => /[\.\/a-zA-Z0-9_-]+\.loom/,

    enum_definition: ($) =>
      seq(
        optional("export"),
        "enum",
        field("name", $.identifier),
        "{",
        optional($._newline),
        multilineCommaSep($.enum_variant),
        optional($._newline),
        "}",
      ),

    enum_variant: ($) =>
      seq(
        field("name", $.identifier),
        optional(seq("=", field("value", choice($.integer, $.string_literal)))),
      ),

    variable_declaration: ($) =>
      seq(
        repeat($._modifier),
        field("keyword", choice("let", "const")),
        field("name", $.identifier),
        ":",
        field("type", $.type),
        "=",
        field("value", $._expression),
      ),

    assignment: ($) =>
      seq(
        field("name", $.identifier),
        repeat(seq("[", field("index", $._expression), "]")),
        "=",
        field("value", $._expression),
      ),

    parameter: ($) =>
      seq(field("name", $.identifier), ":", field("type", $.type)),

    function_definition: ($) =>
      seq(
        optional(
          seq("#", field("tag", $.namespaced_arg), optional($._newline)),
        ),
        repeat($._modifier),
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

    while: ($) =>
      seq(
        "while",
        field("condition", $._expression),
        field("block", $.block),
      ),

    do_while: ($) =>
      seq(
        "do",
        field("block", $.block),
        "while",
        field("condition", $._expression),
      ),

    for: ($) =>
      seq(
        "for",
        field("iterator", $.identifier),
        "in",
        field("start", $._expression),
        "..",
        field("end", $._expression),
        field("block", $.block),
      ),

    contextModifier: ($) =>
      choice(
        seq("as", field("selector", $.selector)),
        seq("at", field("selector", $.selector)),
        seq("align", field("axes", $.swizzle)),
        seq("anchored", field("anchor", choice("eyes", "feet"))),
        seq("facing", field("pos", $.vec3)),
        seq(
          "facing",
          "entity",
          field("selector", $.selector),
          field("anchor", choice("eyes", "feet")),
        ),
        seq(
          "in",
          field("dim", $.namespaced_arg),
        ),
        seq(
          "on",
          field(
            "relation",
            choice(
              "attacker",
              "controller",
              "leasher",
              "origin",
              "owner",
              "passengers",
              "target",
              "vehicle",
            ),
          ),
        ),
        seq("positioned", field("pos", $.vec3)),
        seq("positioned", "as", field("selector", $.selector)),
        seq(
          "positioned",
          "over",
          field(
            "heightmap",
            choice(
              "world_surface",
              "motion_blocking",
              "motion_blocking_no_leaves",
              "ocean_floor",
            ),
          ),
        ),
        seq("rotated", field("rot", $.vec2)),
        seq("rotated", "as", field("selector", $.selector)),
      ),

    context_statement: ($) =>
      seq(
        repeat1($.contextModifier),
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
          repeat(choice($.command_arg, $.interpolation, $.integer, "$")),
        ),
      ),

    interpolation: ($) =>
      seq(
        "${",
        field("expression", $._expression),
        "}",
      ),

    _expression: ($) =>
      choice(
        $.member_expression,
        $.binary_expression,
        $.unary_expression,
        $.slice_expression,
        $.element_expression,
        $.function_call,
        $.variable_ref,
        $.integer,
        $.boolean,
        $.string_literal,
        $.parenthesized_expression,
        $.list_expression,
      ),

    member_expression: ($) =>
      prec(
        8,
        seq(
          field("object", $.identifier),
          ".",
          field("property", $.identifier),
        ),
      ),

    slice_expression: ($) =>
      prec(
        7,
        seq(
          field("target", $._expression),
          "[",
          field("start", $._expression),
          "..",
          field("end", $._expression),
          "]",
        ),
      ),

    element_expression: ($) =>
      prec(
        7,
        seq(
          field("target", $._expression),
          "[",
          field("index", $._expression),
          "]",
        ),
      ),

    parenthesized_expression: ($) => seq("(", $._expression, ")"),

    binary_expression: ($) =>
      choice(
        prec.left(
          6,
          seq(
            field("left", $.namespaced_arg),
            field("operator", "at"),
            field("right", $.vec3),
          ),
        ),
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
        choice(
          seq(
            field("operator", choice("-", "!")),
            field("argument", $._expression),
          ),
          seq(
            field("operator", "entity"),
            field("argument", $.selector),
          ),
        ),
      ),

    list_expression: ($) => seq("[", commaSep($._expression), "]"),

    function_call: ($) =>
      seq(
        field("name", $.identifier),
        "(",
        field("arguments", commaSep($._expression)),
        ")",
      ),

    variable_ref: ($) => prec(2, field("name", $.identifier)),

    selector: ($) =>
      choice(
        seq(
          choice("@s", "@r", "@p", "@e", "@a", "@n"),
          optional(
            seq(
              "[",
              repeat(choice(
                $._selector_safe_content,
                $.selector_nbt,
              )),
              "]",
            ),
          ),
        ),
        /[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}/,
        /[a-zA-Z0-9_]{3,16}/,
      ),
    selector_nbt: ($) =>
      seq(
        "{",
        repeat(choice(
          /[^{}]+/,
          $.selector_nbt,
        )),
        "}",
      ),

    _selector_safe_content: ($) =>
      choice(
        /[^\[\]{}'"]+/,
        $.string_literal,
      ),

    swizzle: () => /x|y|z|xy|xz|yx|yz|zx|zy|xyz|xzy|yxz|yzx|zxy|zyx/,

    identifier: () => /[a-z_][a-z0-9_]*/i,

    type: ($) => choice(/[a-z]+/i, $.list_type),
    list_type: ($) => seq($.type, "[]"),

    string_literal: ($) =>
      choice(
        $._string_double,
        $._string_single,
      ),

    _string_double: ($) =>
      seq(
        '"',
        repeat(choice(
          /[^"\\]+/,
          $._escape_sequence,
        )),
        '"',
      ),

    _string_single: ($) =>
      seq(
        "'",
        repeat(choice(
          /[^'\\]+/,
          $._escape_sequence,
        )),
        "'",
      ),

    _escape_sequence: () => /\\(["'\\bfnrtv])/,

    vec3: ($) =>
      seq(
        $._coordinate_component,
        $._coordinate_component,
        $._coordinate_component,
      ),

    vec2: ($) =>
      seq(
        $._coordinate_component,
        $._coordinate_component,
      ),

    _coordinate_component: () =>
      /([~^]-?([0-9]+(\.[0-9]+)?)?|-?[0-9]+(\.[0-9]+)?)/,

    integer: () => /\d+/,

    boolean: () => choice("true", "false"),

    command_arg: () =>
      token(prec(
        -1,
        choice(
          /[^\s;$]+/,
          seq("$", /[^\s;{]+/),
        ),
      )),

    namespaced_arg: ($) => seq(optional(seq($.identifier, ":")), $.identifier),

    _modifier: () => choice("export", "extern"),

    comment: () => token(seq("--", /.*/)),
  },
});
