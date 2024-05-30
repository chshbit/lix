#include <sstream>          // for basic_ostringstream, basic_ios, basic_os...
#include "eval.hh"          // for PrimOp, EvalState
#include "json-to-value.hh" // for JSONParseError, parseJSON
#include "pos-idx.hh"       // for PosIdx
#include "pos-table.hh"     // for PosTable
#include "value-to-json.hh" // for printValueAsJSON
#include "value.hh"         // for Value
#include "value/context.hh" // for NixStringContext

namespace nix {

/**
 * builtins.fromJSON
 */

static void prim_fromJSON(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto s = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the first argument passed to builtins.fromJSON"
    );
    try {
        parseJSON(state, s, v);
    } catch (JSONParseError & e) {
        e.addTrace(state.positions[pos], "while decoding a JSON string");
        throw;
    }
}

PrimOp primop_fromJSON({
    .name = "__fromJSON",
    .args = {"e"},
    .doc = R"(
      Convert a JSON string to a Nix value. For example,

      ```nix
      builtins.fromJSON ''{"x": [1, 2, 3], "y": null}''
      ```

      returns the value `{ x = [ 1 2 3 ]; y = null; }`.
    )",
    .fun = prim_fromJSON,
});

/**
 * builtins.toJSON
 */

static void prim_toJSON(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    std::ostringstream out;
    NixStringContext context;
    printValueAsJSON(state, true, *args[0], pos, out, context);
    v.mkString(out.str(), context);
}

PrimOp primop_toJSON({
    .name = "__toJSON",
    .args = {"e"},
    .doc = R"(
      Return a string containing a JSON representation of *e*. Strings,
      integers, floats, booleans, nulls and lists are mapped to their JSON
      equivalents. Sets (except derivations) are represented as objects.
      Derivations are translated to a JSON string containing the
      derivation’s output path. Paths are copied to the store and
      represented as a JSON string of the resulting store path.
    )",
    .fun = prim_toJSON,
});

}
