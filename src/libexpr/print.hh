// SPDX-FileCopyrightText: 2024 Nix and Lix Authors
//
// SPDX-License-Identifier: LGPL-2.1-only

#pragma once
/**
 * @file
 * @brief Common printing functions for the Nix language
 *
 * While most types come with their own methods for printing, they share some
 * functions that are placed here.
 */

#include <iostream>

#include "fmt.hh"
#include "print-options.hh"
#include "print-elided.hh"

namespace nix {

struct Expr;

class EvalState;
struct Value;

/** Print `true` or `false`. */
std::ostream & printLiteralBool(std::ostream & o, bool b);

/**
 * Print a string as an attribute name in the Nix expression language syntax.
 *
 * Prints a quoted string if necessary.
 */
std::ostream & printAttributeName(std::ostream & o, std::string_view s);

/**
 * Returns `true' is a string is a reserved keyword which requires quotation
 * when printing attribute set field names.
 */
bool isReservedKeyword(const std::string_view str);

/**
 * Print a string as an identifier in the Nix expression language syntax.
 *
 * FIXME: "identifier" is ambiguous. Identifiers do not have a single
 *        textual representation. They can be used in variable references,
 *        let bindings, left-hand sides or attribute names in a select
 *        expression, or something else entirely, like JSON. Use one of the
 *        `print*` functions instead.
 */
std::ostream & printIdentifier(std::ostream & o, std::string_view s);

void printValue(EvalState & state, std::ostream & str, Value & v, PrintOptions options = PrintOptions {});

/**
 * A partially-applied form of `printValue` which can be formatted using `<<`
 * without allocating an intermediate string.
 * This class should not outlive the eval state or it will UAF.
 * FIXME: This should take `nix::ref`s, to avoid that, but our eval methods all have
 * EvalState &, not ref<EvalState>, and constructing a new shared_ptr to data that
 * already has a shared_ptr is a much bigger footgun. In the current architecture of
 * libexpr, using a ValuePrinter after an EvalState has been destroyed would be
 * pretty hard.
 */
class ValuePrinter {
    friend std::ostream & operator << (std::ostream & output, const ValuePrinter & printer);
private:
    EvalState & state;
    Value & value;
    PrintOptions options;

public:
    ValuePrinter(EvalState & state, Value & value, PrintOptions options = PrintOptions {})
        : state(state), value(value), options(options) { }
};

std::ostream & operator<<(std::ostream & output, const ValuePrinter & printer);


/**
 * `ValuePrinter` does its own ANSI formatting, so we don't color it
 * magenta.
 */
template<>
fmt_internal::HintFmt & fmt_internal::HintFmt::operator%(const ValuePrinter & value);

/**
 * A partially-applied form of Expr::show(), which can be formatted using `<<`
 * without allocating an intermediate string.
 * This class should not outlive the eval state or it will UAF.
 * FIXME: This should take `nix::ref`s, to avoid that, but our eval methods all have
 * EvalState &, not ref<EvalState>, and constructing a new shared_ptr to data that
 * already has a shared_ptr is a much bigger footgun. In the current architecture of
 * libexpr, using an ExprPrinter after an EvalState has been destroyed would be
 * pretty hard.
 */
class ExprPrinter
{
    /** The eval state used to get symbols. */
    EvalState const & state;
    /** The expression to print. */
    Expr const & expr;

public:
    ExprPrinter(EvalState const & state, Expr const & expr) : state(state), expr(expr) { }
    friend std::ostream & operator << (std::ostream & output, ExprPrinter const & printer);
};

}
