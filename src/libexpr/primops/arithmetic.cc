#include "primops.hh"

namespace nix {

/**
 * builtins.add
 */

static void prim_add(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceValue(*args[0], pos);
    state.forceValue(*args[1], pos);
    if (args[0]->type() == nFloat || args[1]->type() == nFloat) {
        v.mkFloat(
            state.forceFloat(*args[0], pos, "while evaluating the first argument of the addition")
            + state.forceFloat(
                *args[1], pos, "while evaluating the second argument of the addition"
            )
        );
    } else {
        v.mkInt(
            state.forceInt(*args[0], pos, "while evaluating the first argument of the addition")
            + state.forceInt(*args[1], pos, "while evaluating the second argument of the addition")
        );
    }
}

PrimOp primop_add({
    .name = "__add",
    .args = {"e1", "e2"},
    .doc = R"(
      Return the sum of the numbers *e1* and *e2*.
      Return a float if either *e1* or *e2* is a float, otherwise
      return an integer.
    )",
    .fun = prim_add,
});

/**
 * builtins.bitAnd
 */

static void prim_bitAnd(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    v.mkInt(
        state.forceInt(
            *args[0], pos, "while evaluating the first argument passed to builtins.bitAnd"
        )
        & state.forceInt(
            *args[1], pos, "while evaluating the second argument passed to builtins.bitAnd"
        )
    );
}

PrimOp primop_bitAnd({
    .name = "__bitAnd",
    .args = {"e1", "e2"},
    .doc = R"(
      Return the bitwise AND of the integers *e1* and *e2*.
    )",
    .fun = prim_bitAnd,
});

/**
 * builtins.bitOr
 */

static void prim_bitOr(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    v.mkInt(
        state.forceInt(
            *args[0], pos, "while evaluating the first argument passed to builtins.bitOr"
        )
        | state.forceInt(
            *args[1], pos, "while evaluating the second argument passed to builtins.bitOr"
        )
    );
}

PrimOp primop_bitOr({
    .name = "__bitOr",
    .args = {"e1", "e2"},
    .doc = R"(
      Return the bitwise OR of the integers *e1* and *e2*.
    )",
    .fun = prim_bitOr,
});

/**
 * builtins.bitXor
 */

static void prim_bitXor(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    v.mkInt(
        state.forceInt(
            *args[0], pos, "while evaluating the first argument passed to builtins.bitXor"
        )
        ^ state.forceInt(
            *args[1], pos, "while evaluating the second argument passed to builtins.bitXor"
        )
    );
}

PrimOp primop_bitXor({
    .name = "__bitXor",
    .args = {"e1", "e2"},
    .doc = R"(
      Return the bitwise XOR of the integers *e1* and *e2*.
    )",
    .fun = prim_bitXor,
});

/**
 * builtins.ceil
 */

static void prim_ceil(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto value = state.forceFloat(
        *args[0],
        args[0]->determinePos(pos),
        "while evaluating the first argument passed to builtins.ceil"
    );
    v.mkInt(ceil(value));
}

PrimOp primop_ceil({
    .name = "__ceil",
    .args = {"double"},
    .doc = R"(
        Converts an IEEE-754 double-precision floating-point number (*double*) to
        the next higher integer.

        If the datatype is neither an integer nor a "float", an evaluation error will be
        thrown.
    )",
    .fun = prim_ceil,
});

/**
 * builtins.div
 */

static void prim_div(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceValue(*args[0], pos);
    state.forceValue(*args[1], pos);

    NixFloat f2 =
        state.forceFloat(*args[1], pos, "while evaluating the second operand of the division");
    if (f2 == 0) {
        state.error<EvalError>("division by zero").atPos(pos).debugThrow();
    }

    if (args[0]->type() == nFloat || args[1]->type() == nFloat) {
        v.mkFloat(
            state.forceFloat(*args[0], pos, "while evaluating the first operand of the division")
            / f2
        );
    } else {
        NixInt i1 =
            state.forceInt(*args[0], pos, "while evaluating the first operand of the division");
        NixInt i2 =
            state.forceInt(*args[1], pos, "while evaluating the second operand of the division");
        /* Avoid division overflow as it might raise SIGFPE. */
        if (i1 == std::numeric_limits<NixInt>::min() && i2 == -1) {
            state.error<EvalError>("overflow in integer division").atPos(pos).debugThrow();
        }

        v.mkInt(i1 / i2);
    }
}

PrimOp primop_div({
    .name = "__div",
    .args = {"e1", "e2"},
    .doc = R"(
      Return the quotient of the numbers *e1* and *e2*.
    )",
    .fun = prim_div,
});

/**
 * builtins.floor
 */

static void prim_floor(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto value = state.forceFloat(
        *args[0],
        args[0]->determinePos(pos),
        "while evaluating the first argument passed to builtins.floor"
    );
    v.mkInt(floor(value));
}

PrimOp primop_floor({
    .name = "__floor",
    .args = {"double"},
    .doc = R"(
        Converts an IEEE-754 double-precision floating-point number (*double*) to
        the next lower integer.

        If the datatype is neither an integer nor a "float", an evaluation error will be
        thrown.
    )",
    .fun = prim_floor,
});

/**
 * builtins.lessThan
 */

void prim_lessThan(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceValue(*args[0], pos);
    state.forceValue(*args[1], pos);
    // pos is exact here, no need for a message.
    CompareValues comp(state, noPos, "");
    v.mkBool(comp(args[0], args[1]));
}

PrimOp primop_lessThan({
    .name = "__lessThan",
    .args = {"e1", "e2"},
    .doc = R"(
      Return `true` if the number *e1* is less than the number *e2*, and
      `false` otherwise. Evaluation aborts if either *e1* or *e2* does not
      evaluate to a number.
    )",
    .fun = prim_lessThan,
});

/**
 * builtins.mul
 */

static void prim_mul(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceValue(*args[0], pos);
    state.forceValue(*args[1], pos);
    if (args[0]->type() == nFloat || args[1]->type() == nFloat) {
        v.mkFloat(
            state.forceFloat(*args[0], pos, "while evaluating the first of the multiplication")
            * state.forceFloat(
                *args[1], pos, "while evaluating the second argument of the multiplication"
            )
        );
    } else {
        v.mkInt(
            state.forceInt(
                *args[0], pos, "while evaluating the first argument of the multiplication"
            )
            * state.forceInt(
                *args[1], pos, "while evaluating the second argument of the multiplication"
            )
        );
    }
}

PrimOp primop_mul({
    .name = "__mul",
    .args = {"e1", "e2"},
    .doc = R"(
      Return the product of the numbers *e1* and *e2*.
    )",
    .fun = prim_mul,
});

/**
 * builtins.sub
 */

static void prim_sub(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceValue(*args[0], pos);
    state.forceValue(*args[1], pos);
    if (args[0]->type() == nFloat || args[1]->type() == nFloat) {
        v.mkFloat(
            state.forceFloat(
                *args[0], pos, "while evaluating the first argument of the subtraction"
            )
            - state.forceFloat(
                *args[1], pos, "while evaluating the second argument of the subtraction"
            )
        );
    } else {
        v.mkInt(
            state.forceInt(*args[0], pos, "while evaluating the first argument of the subtraction")
            - state.forceInt(
                *args[1], pos, "while evaluating the second argument of the subtraction"
            )
        );
    }
}

PrimOp primop_sub({
    .name = "__sub",
    .args = {"e1", "e2"},
    .doc = R"(
      Return the difference between the numbers *e1* and *e2*.
    )",
    .fun = prim_sub,
});
}
