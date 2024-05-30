#pragma once
///@file

#include "derivations.hh"
#include "eval.hh"

#include <regex>
#include <vector>

namespace nix {

/**
 * For functions where we do not expect deep recursion, we can use a sizable
 * part of the stack a free allocation space.
 *
 * Note: this is expected to be multiplied by sizeof(Value), or about 24 bytes.
 */
constexpr size_t nonRecursiveStackReservation = 128;

/**
 * Functions that maybe applied to self-similar inputs, such as concatMap on a
 * tree, should reserve a smaller part of the stack for allocation.
 *
 * Note: this is expected to be multiplied by sizeof(Value), or about 24 bytes.
 */
constexpr size_t conservativeStackReservation = 16;

struct RegisterPrimOp
{
    typedef std::vector<PrimOp> PrimOps;
    static PrimOps * primOps;

    /**
     * You can register a constant by passing an arity of 0. fun
     * will get called during EvalState initialization, so there
     * may be primops not yet added and builtins is not yet sorted.
     */
    RegisterPrimOp(PrimOp && primOp);
};

/* These primops are disabled without enableNativeCode, but plugins
   may wish to use them in limited contexts without globally enabling
   them. */

/**
 * Load a ValueInitializer from a DSO and return whatever it initializes
 */
void prim_importNative(EvalState & state, const PosIdx pos, Value ** args, Value & v);

/**
 * Execute a program and parse its output
 */
void prim_exec(EvalState & state, const PosIdx pos, Value ** args, Value & v);

void prim_lessThan(EvalState & state, const PosIdx pos, Value ** args, Value & v);

void prim_importNative(EvalState & state, const PosIdx pos, Value ** args, Value & v);

void prim_trace(EvalState & state, const PosIdx pos, Value ** args, Value & v);

void prim_second(EvalState & state, const PosIdx pos, Value ** args, Value & v);

void makePositionThunks(EvalState & state, const PosIdx pos, Value & line, Value & column);

void mkOutputString(
    EvalState & state,
    BindingsBuilder & attrs,
    const StorePath & drvPath,
    const std::pair<std::string, DerivationOutput> & o
);

#if HAVE_BOEHMGC
typedef std::list<Value *, gc_allocator<Value *>> ValueList;
#else
typedef std::list<Value *> ValueList;
#endif

/**
 * getAttr wrapper
 */

Bindings::iterator
getAttr(EvalState & state, Symbol attrSym, Bindings * attrSet, std::string_view errorCtx);

/**
 * Struct definitions
 */

struct RegexCache
{
    // TODO use C++20 transparent comparison when available
    std::unordered_map<std::string_view, std::regex> cache;
    std::list<std::string> keys;

    std::regex get(std::string_view re)
    {
        auto it = cache.find(re);
        if (it != cache.end()) {
            return it->second;
        }
        keys.emplace_back(re);
        return cache.emplace(keys.back(), std::regex(keys.back(), std::regex::extended))
            .first->second;
    }
};

struct CompareValues
{
    EvalState & state;
    const PosIdx pos;
    const std::string_view errorCtx;

    CompareValues(EvalState & state, const PosIdx pos, const std::string_view && errorCtx)
        : state(state)
        , pos(pos)
        , errorCtx(errorCtx){};

    bool operator()(Value * v1, Value * v2) const
    {
        return (*this)(v1, v2, errorCtx);
    }

    bool operator()(Value * v1, Value * v2, std::string_view errorCtx) const
    {
        try {
            if (v1->type() == nFloat && v2->type() == nInt) {
                return v1->fpoint < v2->integer;
            }
            if (v1->type() == nInt && v2->type() == nFloat) {
                return v1->integer < v2->fpoint;
            }
            if (v1->type() != v2->type()) {
                state.error<EvalError>("cannot compare %s with %s", showType(*v1), showType(*v2))
                    .debugThrow();
            }
// Allow selecting a subset of enum values
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wswitch-enum"
            switch (v1->type()) {
            case nInt:
                return v1->integer < v2->integer;
            case nFloat:
                return v1->fpoint < v2->fpoint;
            case nString:
                return strcmp(v1->string.s, v2->string.s) < 0;
            case nPath:
                return strcmp(v1->_path, v2->_path) < 0;
            case nList:
                // Lexicographic comparison
                for (size_t i = 0;; i++) {
                    if (i == v2->listSize()) {
                        return false;
                    } else if (i == v1->listSize()) {
                        return true;
                    } else if (!state.eqValues(
                                   *v1->listElems()[i], *v2->listElems()[i], pos, errorCtx
                               ))
                    {
                        return (*this)(
                            v1->listElems()[i],
                            v2->listElems()[i],
                            "while comparing two list elements"
                        );
                    }
                }
            default:
                state
                    .error<EvalError>(
                        "cannot compare %s with %s; values of that type are incomparable",
                        showType(*v1),
                        showType(*v2)
                    )
                    .debugThrow();
#pragma GCC diagnostic pop
            }
        } catch (Error & e) {
            if (!errorCtx.empty()) {
                e.addTrace(nullptr, errorCtx);
            }
            throw;
        }
    }
};

struct RealisePathFlags
{
    // Whether to check that the path is allowed in pure eval mode
    bool checkForPureEval = true;
};

SourcePath
realisePath(EvalState & state, const PosIdx pos, Value & v, const RealisePathFlags flags = {});

}
