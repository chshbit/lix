#include "eval-settings.hh"
#include "primops.hh"

namespace nix {

// access to exact position information (ie, line and colum numbers) is deferred
// due to the cost associated with calculating that information and how rarely
// it is used in practice. this is achieved by creating thunks to otherwise
// inaccessible primops that are not exposed as __op or under builtins to turn
// the internal PosIdx back into a line and column number, respectively. exposing
// these primops in any way would at best be not useful and at worst create wildly
// indeterministic eval results depending on parse order of files.
//
// in a simpler world this would instead be implemented as another kind of thunk,
// but each type of thunk has an associated runtime cost in the current evaluator.
// as with black holes this cost is too high to justify another thunk type to check
// for in the very hot path that is forceValue.
static struct LazyPosAcessors
{
    PrimOp primop_lineOfPos{
        .arity = 1,
        .fun = [](EvalState & state, PosIdx pos, Value ** args, Value & v
               ) { v.mkInt(state.positions[PosIdx(args[0]->integer)].line); }
    };
    PrimOp primop_columnOfPos{
        .arity = 1,
        .fun = [](EvalState & state, PosIdx pos, Value ** args, Value & v
               ) { v.mkInt(state.positions[PosIdx(args[0]->integer)].column); }
    };

    Value lineOfPos, columnOfPos;

    LazyPosAcessors()
    {
        lineOfPos.mkPrimOp(&primop_lineOfPos);
        columnOfPos.mkPrimOp(&primop_columnOfPos);
    }

    void operator()(EvalState & state, const PosIdx pos, Value & line, Value & column)
    {
        Value * posV = state.allocValue();
        posV->mkInt(pos.id);
        line.mkApp(&lineOfPos, posV);
        column.mkApp(&columnOfPos, posV);
    }
} makeLazyPosAccessors;

void makePositionThunks(EvalState & state, const PosIdx pos, Value & line, Value & column)
{
    makeLazyPosAccessors(state, pos, line, column);
}

/**
 * Takes two arguments and evaluates to the second one. Used as the
 * builtins.traceVerbose implementation when --trace-verbose is not enabled
 */

void prim_second(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceValue(*args[1], pos);
    v = *args[1];
}

/**
 * builtins.deepSeq
 */

static void prim_deepSeq(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceValueDeep(*args[0]);
    state.forceValue(*args[1], pos);
    v = *args[1];
}

PrimOp primop_deepSeq({
    .name = "__deepSeq",
    .args = {"e1", "e2"},
    .doc = R"(
      This is like `seq e1 e2`, except that *e1* is evaluated *deeply*:
      if it’s a list or set, its elements or attributes are also
      evaluated recursively.
    )",
    .fun = prim_deepSeq,
});

/**
 * builtins.seq
 */

static void prim_seq(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceValue(*args[0], pos);
    state.forceValue(*args[1], pos);
    v = *args[1];
}

PrimOp primop_seq({
    .name = "__seq",
    .args = {"e1", "e2"},
    .doc = R"(
      Evaluate *e1*, then evaluate and return *e2*. This ensures that a
      computation is strict in the value of *e1*.
    )",
    .fun = prim_seq,
});

/**
 * builtins.trace
 */

void prim_trace(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceValue(*args[0], pos);
    if (args[0]->type() == nString) {
        printError("trace: %1%", args[0]->string.s);
    } else {
        printError("trace: %1%", ValuePrinter(state, *args[0]));
    }
    if (evalSettings.builtinsTraceDebugger && state.debugRepl && !state.debugTraces.empty()) {
        const DebugTrace & last = state.debugTraces.front();
        state.runDebugRepl(nullptr, last.env, last.expr);
    }
    state.forceValue(*args[1], pos);
    v = *args[1];
}

PrimOp primop_trace({
    .name = "__trace",
    .args = {"e1", "e2"},
    .doc = R"(
      Evaluate *e1* and print its abstract syntax representation on
      standard error. Then return *e2*. This function is useful for
      debugging.

      If the
      [`debugger-on-trace`](@docroot@/command-ref/conf-file.md#conf-debugger-on-trace)
      option is set to `true` and the `--debugger` flag is given, the
      interactive debugger will be started when `trace` is called (like
      [`break`](@docroot@/language/builtins.md#builtins-break)).
    )",
    .fun = prim_trace,
});

/**
 * builtins.unsafeGetAttrPos
 */

static void prim_unsafeGetAttrPos(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto attr = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the first argument passed to builtins.unsafeGetAttrPos"
    );
    state.forceAttrs(
        *args[1], pos, "while evaluating the second argument passed to builtins.unsafeGetAttrPos"
    );
    Bindings::iterator i = args[1]->attrs->find(state.symbols.create(attr));
    if (i == args[1]->attrs->end()) {
        v.mkNull();
    } else {
        state.mkPos(v, i->pos);
    }
}

PrimOp primop_unsafeGetAttrPos(PrimOp{
    .name = "__unsafeGetAttrPos",
    .arity = 2,
    .fun = prim_unsafeGetAttrPos,
});

}
