#pragma once
///@file

#include "eval.hh"

#include <regex>
#include <tuple>
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

void makePositionThunks(EvalState & state, const PosIdx pos, Value & line, Value & column);

#if HAVE_BOEHMGC
typedef std::list<Value *, gc_allocator<Value *>> ValueList;
#else
typedef std::list<Value *> ValueList;
#endif

/**
 * getAttr wrapper
 */

Bindings::iterator getAttr(EvalState & state, Symbol attrSym, Bindings * attrSet, std::string_view errorCtx);

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
        if (it != cache.end())
            return it->second;
        keys.emplace_back(re);
        return cache.emplace(keys.back(), std::regex(keys.back(), std::regex::extended)).first->second;
    }
};

struct RealisePathFlags {
    // Whether to check that the path is allowed in pure eval mode
    bool checkForPureEval = true;
};

SourcePath realisePath(EvalState & state, const PosIdx pos, Value & v, const RealisePathFlags flags = {});

}
