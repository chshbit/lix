#include "eval-settings.hh"
#include "primops.hh"

namespace nix {

/**
 * builtins.exec
 */

void prim_exec(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceList(*args[0], pos, "while evaluating the first argument passed to builtins.exec");
    auto elems = args[0]->listElems();
    auto count = args[0]->listSize();
    if (count == 0) {
        state.error<EvalError>("at least one argument to 'exec' required").atPos(pos).debugThrow();
    }
    NixStringContext context;
    auto program =
        state
            .coerceToString(
                pos,
                *elems[0],
                context,
                "while evaluating the first element of the argument passed to builtins.exec",
                false,
                false
            )
            .toOwned();
    Strings commandArgs;
    for (unsigned int i = 1; i < args[0]->listSize(); ++i) {
        commandArgs.push_back(
            state
                .coerceToString(
                    pos,
                    *elems[i],
                    context,
                    "while evaluating an element of the argument passed to builtins.exec",
                    false,
                    false
                )
                .toOwned()
        );
    }
    try {
        auto _ = state.realiseContext(context); // FIXME: Handle CA derivations
    } catch (InvalidPathError & e) {
        state
            .error<EvalError>(
                "cannot execute '%1%', since path '%2%' is not valid", program, e.path
            )
            .atPos(pos)
            .debugThrow();
    }

    auto output = runProgram(program, true, commandArgs);
    Expr * parsed;
    try {
        parsed = state.parseExprFromString(std::move(output), state.rootPath(CanonPath::root));
    } catch (Error & e) {
        e.addTrace(state.positions[pos], "while parsing the output from '%1%'", program);
        throw;
    }
    try {
        state.eval(parsed, v);
    } catch (Error & e) {
        e.addTrace(state.positions[pos], "while evaluating the output from '%1%'", program);
        throw;
    }
}

/**
 * builtins.getEnv
 */

static void prim_getEnv(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    std::string name(state.forceStringNoCtx(
        *args[0], pos, "while evaluating the first argument passed to builtins.getEnv"
    ));
    v.mkString(evalSettings.restrictEval || evalSettings.pureEval ? "" : getEnv(name).value_or(""));
}

static RegisterPrimOp primop_getEnv({
    .name = "__getEnv",
    .args = {"s"},
    .doc = R"(
      `getEnv` returns the value of the environment variable *s*, or an
      empty string if the variable doesn’t exist. This function should be
      used with care, as it can introduce all sorts of nasty environment
      dependencies in your Nix expression.

      `getEnv` is used in Nix Packages to locate the file
      `~/.nixpkgs/config.nix`, which contains user-local settings for Nix
      Packages. (That is, it does a `getEnv "HOME"` to locate the user’s
      home directory.)
    )",
    .fun = prim_getEnv,
});

}
