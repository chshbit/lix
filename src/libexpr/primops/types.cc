#include "primops.hh"

namespace nix {

/**
 * Generic isType function
 */

static inline auto prim_isType(auto n)
{
    return [n](EvalState & state, const PosIdx pos, Value ** args, Value & v) {
        state.forceValue(*args[0], pos);
        v.mkBool(args[0]->type() == n);
    };
}

/**
 * builtins.functionArgs
 */

static void prim_functionArgs(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceValue(*args[0], pos);
    if (args[0]->isPrimOpApp() || args[0]->isPrimOp()) {
        v.mkAttrs(&state.emptyBindings);
        return;
    }
    if (!args[0]->isLambda()) {
        state.error<TypeError>("'functionArgs' requires a function").atPos(pos).debugThrow();
    }

    if (!args[0]->lambda.fun->hasFormals()) {
        v.mkAttrs(&state.emptyBindings);
        return;
    }

    auto attrs = state.buildBindings(args[0]->lambda.fun->formals->formals.size());
    for (auto & i : args[0]->lambda.fun->formals->formals) {
        // !!! should optimise booleans (allocate only once)
        attrs.alloc(i.name, i.pos).mkBool(i.def);
    }
    v.mkAttrs(attrs);
}

PrimOp primop_functionArgs({
    .name = "__functionArgs",
    .args = {"f"},
    .doc = R"(
      Return a set containing the names of the formal arguments expected
      by the function *f*. The value of each attribute is a Boolean
      denoting whether the corresponding argument has a default value. For
      instance, `functionArgs ({ x, y ? 123}: ...) = { x = false; y =
      true; }`.

      "Formal argument" here refers to the attributes pattern-matched by
      the function. Plain lambdas are not included, e.g. `functionArgs (x:
      ...) = { }`.
    )",
    .fun = prim_functionArgs,
});

/**
 * builtins.isAttrs
 */

PrimOp primop_isAttrs({
    .name = "__isAttrs",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a set, and `false` otherwise.
    )",
    .fun = prim_isType(nAttrs),
});

/**
 * builtins.isBool
 */

PrimOp primop_isBool({
    .name = "__isBool",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a bool, and `false` otherwise.
    )",
    .fun = prim_isType(nBool),
});

/**
 * builtins.Float
 */

PrimOp primop_isFloat({
    .name = "__isFloat",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a float, and `false` otherwise.
    )",
    .fun = prim_isType(nFloat),
});

/**
 * builtins.isFunction
 */

PrimOp primop_isFunction({
    .name = "__isFunction",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a function, and `false` otherwise.
    )",
    .fun = prim_isType(nFunction),
});

/**
 * builtins.isInt
 */

PrimOp primop_isInt({
    .name = "__isInt",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to an integer, and `false` otherwise.
    )",
    .fun = prim_isType(nInt),
});

/**
 * builtins.isList
 */

PrimOp primop_isList({
    .name = "__isList",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a list, and `false` otherwise.
    )",
    .fun = prim_isType(nList),
});

/**
 * builtins.isNull
 */

PrimOp primop_isNull({
    .name = "isNull",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to `null`, and `false` otherwise.

      This is equivalent to `e == null`.
    )",
    .fun = prim_isType(nNull),
});

/**
 * builtins.isPath
 */

PrimOp primop_isPath({
    .name = "__isPath",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a path, and `false` otherwise.
    )",
    .fun = prim_isType(nPath),
});

/**
 * builtins.isString
 */

PrimOp primop_isString({
    .name = "__isString",
    .args = {"e"},
    .doc = R"(
      Return `true` if *e* evaluates to a string, and `false` otherwise.
    )",
    .fun = prim_isType(nString),
});

/**
 * builtins.typeOf
 */

static void prim_typeOf(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceValue(*args[0], pos);
    std::string t;
    switch (args[0]->type()) {
    case nInt:
        t = "int";
        break;
    case nBool:
        t = "bool";
        break;
    case nString:
        t = "string";
        break;
    case nPath:
        t = "path";
        break;
    case nNull:
        t = "null";
        break;
    case nAttrs:
        t = "set";
        break;
    case nList:
        t = "list";
        break;
    case nFunction:
        t = "lambda";
        break;
    case nExternal:
        t = args[0]->external->typeOf();
        break;
    case nFloat:
        t = "float";
        break;
    case nThunk:
        abort();
    }
    v.mkString(t);
}

PrimOp primop_typeOf({
    .name = "__typeOf",
    .args = {"e"},
    .doc = R"(
      Return a string representing the type of the value *e*, namely
      `"int"`, `"bool"`, `"string"`, `"path"`, `"null"`, `"set"`,
      `"list"`, `"lambda"` or `"float"`.
    )",
    .fun = prim_typeOf,
});

}
