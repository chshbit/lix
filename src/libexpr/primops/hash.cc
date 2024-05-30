#include "hash.hh"
#include "eval-error.hh"    // for EvalError, EvalErrorBuilder
#include "eval.hh"          // for EvalState, PrimOp
#include "pos-idx.hh"       // for PosIdx
#include "primops.hh"       // for realisePath
#include "source-path.hh"   // for SourcePath
#include "value.hh"         // for Value
#include "value/context.hh" // for NixStringContext

namespace nix {

/**
 * builtins.hashFile
 */

static void prim_hashFile(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto type = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the first argument passed to builtins.hashFile"
    );
    std::optional<HashType> ht = parseHashType(type);
    if (!ht) {
        state.error<EvalError>("unknown hash type '%1%'", type).atPos(pos).debugThrow();
    }

    auto path = realisePath(state, pos, *args[1]);

    v.mkString(hashString(*ht, path.readFile()).to_string(Base16, false));
}

PrimOp primop_hashFile({
    .name = "__hashFile",
    .args = {"type", "p"},
    .doc = R"(
      Return a base-16 representation of the cryptographic hash of the
      file at path *p*. The hash algorithm specified by *type* must be one
      of `"md5"`, `"sha1"`, `"sha256"` or `"sha512"`.
    )",
    .fun = prim_hashFile,
});

/**
 * builtins.hashString
 */

static void prim_hashString(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto type = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the first argument passed to builtins.hashString"
    );
    std::optional<HashType> ht = parseHashType(type);
    if (!ht) {
        state.error<EvalError>("unknown hash algorithm '%1%'", type).atPos(pos).debugThrow();
    }

    NixStringContext context; // discarded
    auto s = state.forceString(
        *args[1], context, pos, "while evaluating the second argument passed to builtins.hashString"
    );

    v.mkString(hashString(*ht, s).to_string(Base16, false));
}

PrimOp primop_hashString({
    .name = "__hashString",
    .args = {"type", "s"},
    .doc = R"(
      Return a base-16 representation of the cryptographic hash of string
      *s*. The hash algorithm specified by *type* must be one of `"md5"`,
      `"sha1"`, `"sha256"` or `"sha512"`.
    )",
    .fun = prim_hashString,
});

}
