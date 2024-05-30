#include <string.h>                         // for size_t, memcpy, strcmp
#include <algorithm>                        // for sort, min, set_difference
#include <boost/container/small_vector.hpp> // for small_vector
#include <boost/container/vector.hpp>       // for operator+, operator-
#include <iterator>                         // for back_insert_iterator
#include <map>                              // for map, _Rb_tree_iterator
#include <set>                              // for set
#include <string_view>                      // for string_view, operator<=>
#include <utility>                          // for pair
#include <vector>                           // for vector
#include "attr-set.hh"                      // for Attr, Bindings, Bindings...
#include "eval-error.hh"                    // for TypeError, EvalErrorBuilder
#include "eval.hh"                          // for EvalState, PrimOp, prim_...
#include "gc-small-vector.hh"               // for SmallValueVector
#include "pos-idx.hh"                       // for PosIdx, noPos
#include "primops.hh"                       // for CompareValues, ValueList
#include "symbol-table.hh"                  // for Symbol, SymbolTable, Sym...
#include "value.hh"                         // for Value, ValueVector, Valu...

namespace nix {

Bindings::iterator
getAttr(EvalState & state, Symbol attrSym, Bindings * attrSet, std::string_view errorCtx)
{
    Bindings::iterator value = attrSet->find(attrSym);
    if (value == attrSet->end()) {
        state.error<TypeError>("attribute '%s' missing", state.symbols[attrSym])
            .withTrace(noPos, errorCtx)
            .debugThrow();
    }
    return value;
}

/**
 * builtins.attrNames
 */

static void prim_attrNames(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceAttrs(*args[0], pos, "while evaluating the argument passed to builtins.attrNames");

    state.mkList(v, args[0]->attrs->size());

    size_t n = 0;
    for (auto & i : *args[0]->attrs) {
        (v.listElems()[n++] = state.allocValue())->mkString(state.symbols[i.name]);
    }

    std::sort(v.listElems(), v.listElems() + n, [](Value * v1, Value * v2) {
        return strcmp(v1->string.s, v2->string.s) < 0;
    });
}

PrimOp primop_attrNames({
    .name = "__attrNames",
    .args = {"set"},
    .doc = R"(
      Return the names of the attributes in the set *set* in an
      alphabetically sorted list. For instance, `builtins.attrNames { y
      = 1; x = "foo"; }` evaluates to `[ "x" "y" ]`.
    )",
    .fun = prim_attrNames,
});

/**
 * builtins.attrNames
 */

static void prim_attrValues(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceAttrs(*args[0], pos, "while evaluating the argument passed to builtins.attrValues");

    state.mkList(v, args[0]->attrs->size());

    unsigned int n = 0;
    for (auto & i : *args[0]->attrs) {
        v.listElems()[n++] = (Value *) &i;
    }

    std::sort(v.listElems(), v.listElems() + n, [&](Value * v1, Value * v2) {
        std::string_view s1 = state.symbols[((Attr *) v1)->name],
                         s2 = state.symbols[((Attr *) v2)->name];
        return s1 < s2;
    });

    for (unsigned int i = 0; i < n; ++i) {
        v.listElems()[i] = ((Attr *) v.listElems()[i])->value;
    }
}

PrimOp primop_attrValues({
    .name = "__attrValues",
    .args = {"set"},
    .doc = R"(
      Return the values of the attributes in the set *set* in the order
      corresponding to the sorted attribute names.
    )",
    .fun = prim_attrValues,
});

/**
 * builtins.catAttrs
 */

static void prim_catAttrs(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto attrName = state.symbols.create(state.forceStringNoCtx(
        *args[0], pos, "while evaluating the first argument passed to builtins.catAttrs"
    ));
    state.forceList(
        *args[1], pos, "while evaluating the second argument passed to builtins.catAttrs"
    );

    SmallValueVector<nonRecursiveStackReservation> res(args[1]->listSize());
    size_t found = 0;

    for (auto v2 : args[1]->listItems()) {
        state.forceAttrs(
            *v2,
            pos,
            "while evaluating an element in the list passed as second argument to builtins.catAttrs"
        );
        Bindings::iterator i = v2->attrs->find(attrName);
        if (i != v2->attrs->end()) {
            res[found++] = i->value;
        }
    }

    state.mkList(v, found);
    for (unsigned int n = 0; n < found; ++n) {
        v.listElems()[n] = res[n];
    }
}

PrimOp primop_catAttrs({
    .name = "__catAttrs",
    .args = {"attr", "list"},
    .doc = R"(
      Collect each attribute named *attr* from a list of attribute
      sets.  Attrsets that don't contain the named attribute are
      ignored. For example,

      ```nix
      builtins.catAttrs "a" [{a = 1;} {b = 0;} {a = 2;}]
      ```

      evaluates to `[1 2]`.
    )",
    .fun = prim_catAttrs,
});

/**
 * builtins.genericClosure
 */

static void prim_genericClosure(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceAttrs(
        *args[0], noPos, "while evaluating the first argument passed to builtins.genericClosure"
    );

    /* Get the start set. */
    Bindings::iterator startSet = getAttr(
        state,
        state.sStartSet,
        args[0]->attrs,
        "in the attrset passed as argument to builtins.genericClosure"
    );

    state.forceList(
        *startSet->value,
        noPos,
        "while evaluating the 'startSet' attribute passed as argument to builtins.genericClosure"
    );

    ValueList workSet;
    for (auto elem : startSet->value->listItems()) {
        workSet.push_back(elem);
    }

    if (startSet->value->listSize() == 0) {
        v = *startSet->value;
        return;
    }

    /* Get the operator. */
    Bindings::iterator op = getAttr(
        state,
        state.sOperator,
        args[0]->attrs,
        "in the attrset passed as argument to builtins.genericClosure"
    );
    state.forceFunction(
        *op->value,
        noPos,
        "while evaluating the 'operator' attribute passed as argument to builtins.genericClosure"
    );

    /* Construct the closure by applying the operator to elements of
       `workSet', adding the result to `workSet', continuing until
       no new elements are found. */
    ValueList res;
    // `doneKeys' doesn't need to be a GC root, because its values are
    // reachable from res.
    auto cmp = CompareValues(
        state, noPos, "while comparing the `key` attributes of two genericClosure elements"
    );
    std::set<Value *, decltype(cmp)> doneKeys(cmp);
    while (!workSet.empty()) {
        Value * e = *(workSet.begin());
        workSet.pop_front();

        state.forceAttrs(
            *e,
            noPos,
            "while evaluating one of the elements generated by (or initially passed to) "
            "builtins.genericClosure"
        );

        Bindings::iterator key = getAttr(
            state,
            state.sKey,
            e->attrs,
            "in one of the attrsets generated by (or initially passed to) builtins.genericClosure"
        );
        state.forceValue(*key->value, noPos);

        if (!doneKeys.insert(key->value).second) {
            continue;
        }
        res.push_back(e);

        /* Call the `operator' function with `e' as argument. */
        Value newElements;
        state.callFunction(*op->value, 1, &e, newElements, noPos);
        state.forceList(
            newElements,
            noPos,
            "while evaluating the return value of the `operator` passed to builtins.genericClosure"
        );

        /* Add the values returned by the operator to the work set. */
        for (auto elem : newElements.listItems()) {
            state.forceValue(*elem, noPos); // "while evaluating one one of the elements returned by
                                            // the `operator` passed to builtins.genericClosure");
            workSet.push_back(elem);
        }
    }

    /* Create the result list. */
    state.mkList(v, res.size());
    unsigned int n = 0;
    for (auto & i : res) {
        v.listElems()[n++] = i;
    }
}

PrimOp primop_genericClosure(PrimOp{
    .name = "__genericClosure",
    .args = {"attrset"},
    .arity = 1,
    .doc = R"(
      Take an *attrset* with values named `startSet` and `operator` in order to
      return a *list of attrsets* by starting with the `startSet` and recursively
      applying the `operator` function to each `item`. The *attrsets* in the
      `startSet` and the *attrsets* produced by `operator` must contain a value
      named `key` which is comparable. The result is produced by calling `operator`
      for each `item` with a value for `key` that has not been called yet including
      newly produced `item`s. The function terminates when no new `item`s are
      produced. The resulting *list of attrsets* contains only *attrsets* with a
      unique key. For example,

      ```
      builtins.genericClosure {
        startSet = [ {key = 5;} ];
        operator = item: [{
          key = if (item.key / 2 ) * 2 == item.key
               then item.key / 2
               else 3 * item.key + 1;
        }];
      }
      ```
      evaluates to
      ```
      [ { key = 5; } { key = 16; } { key = 8; } { key = 4; } { key = 2; } { key = 1; } ]
      ```
      )",
    .fun = prim_genericClosure,
});

/**
 * builtins.getAttr
 */

void prim_getAttr(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto attr = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the first argument passed to builtins.getAttr"
    );
    state.forceAttrs(
        *args[1], pos, "while evaluating the second argument passed to builtins.getAttr"
    );
    Bindings::iterator i = getAttr(
        state,
        state.symbols.create(attr),
        args[1]->attrs,
        "in the attribute set under consideration"
    );
    // !!! add to stack trace?
    if (state.countCalls && i->pos) {
        state.attrSelects[i->pos]++;
    }
    state.forceValue(*i->value, pos);
    v = *i->value;
}

PrimOp primop_getAttr({
    .name = "__getAttr",
    .args = {"s", "set"},
    .doc = R"(
      `getAttr` returns the attribute named *s* from *set*. Evaluation
      aborts if the attribute doesn’t exist. This is a dynamic version of
      the `.` operator, since *s* is an expression rather than an
      identifier.
    )",
    .fun = prim_getAttr,
});

/**
 * builtins.groupBy
 */

static void prim_groupBy(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceFunction(
        *args[0], pos, "while evaluating the first argument passed to builtins.groupBy"
    );
    state.forceList(
        *args[1], pos, "while evaluating the second argument passed to builtins.groupBy"
    );

    ValueVectorMap attrs;

    for (auto vElem : args[1]->listItems()) {
        Value res;
        state.callFunction(*args[0], *vElem, res, pos);
        auto name = state.forceStringNoCtx(
            res,
            pos,
            "while evaluating the return value of the grouping function passed to builtins.groupBy"
        );
        auto sym = state.symbols.create(name);
        auto vector = attrs.try_emplace(sym, ValueVector()).first;
        vector->second.push_back(vElem);
    }

    auto attrs2 = state.buildBindings(attrs.size());

    for (auto & i : attrs) {
        auto & list = attrs2.alloc(i.first);
        auto size = i.second.size();
        state.mkList(list, size);
        memcpy(list.listElems(), i.second.data(), sizeof(Value *) * size);
    }

    v.mkAttrs(attrs2.alreadySorted());
}

PrimOp primop_groupBy({
    .name = "__groupBy",
    .args = {"f", "list"},
    .doc = R"(
      Groups elements of *list* together by the string returned from the
      function *f* called on each element. It returns an attribute set
      where each attribute value contains the elements of *list* that are
      mapped to the same corresponding attribute name returned by *f*.

      For example,

      ```nix
      builtins.groupBy (builtins.substring 0 1) ["foo" "bar" "baz"]
      ```

      evaluates to

      ```nix
      { b = [ "bar" "baz" ]; f = [ "foo" ]; }
      ```
    )",
    .fun = prim_groupBy,
});

/**
 * builtins.hasAttr
 */

static void prim_hasAttr(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    auto attr = state.forceStringNoCtx(
        *args[0], pos, "while evaluating the first argument passed to builtins.hasAttr"
    );
    state.forceAttrs(
        *args[1], pos, "while evaluating the second argument passed to builtins.hasAttr"
    );
    v.mkBool(args[1]->attrs->find(state.symbols.create(attr)) != args[1]->attrs->end());
}

PrimOp primop_hasAttr({
    .name = "__hasAttr",
    .args = {"s", "set"},
    .doc = R"(
      `hasAttr` returns `true` if *set* has an attribute named *s*, and
      `false` otherwise. This is a dynamic version of the `?` operator,
      since *s* is an expression rather than an identifier.
    )",
    .fun = prim_hasAttr,
});

/**
 * builtins.intersectAttrs
 */

static void prim_intersectAttrs(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceAttrs(
        *args[0], pos, "while evaluating the first argument passed to builtins.intersectAttrs"
    );
    state.forceAttrs(
        *args[1], pos, "while evaluating the second argument passed to builtins.intersectAttrs"
    );

    Bindings & left = *args[0]->attrs;
    Bindings & right = *args[1]->attrs;

    auto attrs = state.buildBindings(std::min(left.size(), right.size()));

    // The current implementation has good asymptotic complexity and is reasonably
    // simple. Further optimization may be possible, but does not seem productive,
    // considering the state of eval performance in 2022.
    //
    // I have looked for reusable and/or standard solutions and these are my
    // findings:
    //
    // STL
    // ===
    // std::set_intersection is not suitable, as it only performs a simultaneous
    // linear scan; not taking advantage of random access. This is O(n + m), so
    // linear in the largest set, which is not acceptable for callPackage in Nixpkgs.
    //
    // Simultaneous scan, with alternating simple binary search
    // ===
    // One alternative algorithm scans the attrsets simultaneously, jumping
    // forward using `lower_bound` in case of inequality. This should perform
    // well on very similar sets, having a local and predictable access pattern.
    // On dissimilar sets, it seems to need more comparisons than the current
    // algorithm, as few consecutive attrs match. `lower_bound` could take
    // advantage of the decreasing remaining search space, but this causes
    // the medians to move, which can mean that they don't stay in the cache
    // like they would with the current naive `find`.
    //
    // Double binary search
    // ===
    // The optimal algorithm may be "Double binary search", which doesn't
    // scan at all, but rather divides both sets simultaneously.
    // See "Fast Intersection Algorithms for Sorted Sequences" by Baeza-Yates et al.
    // https://cs.uwaterloo.ca/~ajsaling/papers/intersection_alg_app10.pdf
    // The only downsides I can think of are not having a linear access pattern
    // for similar sets, and having to maintain a more intricate algorithm.
    //
    // Adaptive
    // ===
    // Finally one could run try a simultaneous scan, count misses and fall back
    // to double binary search when the counter hit some threshold and/or ratio.

    if (left.size() < right.size()) {
        for (auto & l : left) {
            Bindings::iterator r = right.find(l.name);
            if (r != right.end()) {
                attrs.insert(*r);
            }
        }
    } else {
        for (auto & r : right) {
            Bindings::iterator l = left.find(r.name);
            if (l != left.end()) {
                attrs.insert(r);
            }
        }
    }

    v.mkAttrs(attrs.alreadySorted());
}

PrimOp primop_intersectAttrs({
    .name = "__intersectAttrs",
    .args = {"e1", "e2"},
    .doc = R"(
      Return a set consisting of the attributes in the set *e2* which have the
      same name as some attribute in *e1*.

      Performs in O(*n* log *m*) where *n* is the size of the smaller set and *m* the larger set's size.
    )",
    .fun = prim_intersectAttrs,
});

/**
 * builtins.mapAttrs
 */

static void prim_mapAttrs(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceAttrs(
        *args[1], pos, "while evaluating the second argument passed to builtins.mapAttrs"
    );

    auto attrs = state.buildBindings(args[1]->attrs->size());

    for (auto & i : *args[1]->attrs) {
        Value * vName = state.allocValue();
        Value * vFun2 = state.allocValue();
        vName->mkString(state.symbols[i.name]);
        vFun2->mkApp(args[0], vName);
        attrs.alloc(i.name).mkApp(vFun2, i.value);
    }

    v.mkAttrs(attrs.alreadySorted());
}

PrimOp primop_mapAttrs({
    .name = "__mapAttrs",
    .args = {"f", "attrset"},
    .doc = R"(
      Apply function *f* to every element of *attrset*. For example,

      ```nix
      builtins.mapAttrs (name: value: value * 10) { a = 1; b = 2; }
      ```

      evaluates to `{ a = 10; b = 20; }`.
    )",
    .fun = prim_mapAttrs,
});

/**
 * builtins.mapAttrs
 */

static void prim_removeAttrs(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    state.forceAttrs(
        *args[0], pos, "while evaluating the first argument passed to builtins.removeAttrs"
    );
    state.forceList(
        *args[1], pos, "while evaluating the second argument passed to builtins.removeAttrs"
    );

    /* Get the attribute names to be removed.
       We keep them as Attrs instead of Symbols so std::set_difference
       can be used to remove them from attrs[0]. */
    // 64: large enough to fit the attributes of a derivation
    boost::container::small_vector<Attr, 64> names;
    names.reserve(args[1]->listSize());
    for (auto elem : args[1]->listItems()) {
        state.forceStringNoCtx(
            *elem,
            pos,
            "while evaluating the values of the second argument passed to builtins.removeAttrs"
        );
        names.emplace_back(state.symbols.create(elem->string.s), nullptr);
    }
    std::sort(names.begin(), names.end());

    /* Copy all attributes not in that set.  Note that we don't need
       to sort v.attrs because it's a subset of an already sorted
       vector. */
    auto attrs = state.buildBindings(args[0]->attrs->size());
    std::set_difference(
        args[0]->attrs->begin(),
        args[0]->attrs->end(),
        names.begin(),
        names.end(),
        std::back_inserter(attrs)
    );
    v.mkAttrs(attrs.alreadySorted());
}

PrimOp primop_removeAttrs({
    .name = "removeAttrs",
    .args = {"set", "list"},
    .doc = R"(
      Remove the attributes listed in *list* from *set*. The attributes
      don’t have to exist in *set*. For instance,

      ```nix
      removeAttrs { x = 1; y = 2; z = 3; } [ "a" "x" "z" ]
      ```

      evaluates to `{ y = 2; }`.
    )",
    .fun = prim_removeAttrs,
});

/**
 * builtins.zipAttrsWith
 */

static void prim_zipAttrsWith(EvalState & state, const PosIdx pos, Value ** args, Value & v)
{
    // we will first count how many values are present for each given key.
    // we then allocate a single attrset and pre-populate it with lists of
    // appropriate sizes, stash the pointers to the list elements of each,
    // and populate the lists. after that we replace the list in the every
    // attribute with the merge function application. this way we need not
    // use (slightly slower) temporary storage the GC does not know about.

    std::map<Symbol, std::pair<size_t, Value **>> attrsSeen;

    state.forceFunction(
        *args[0], pos, "while evaluating the first argument passed to builtins.zipAttrsWith"
    );
    state.forceList(
        *args[1], pos, "while evaluating the second argument passed to builtins.zipAttrsWith"
    );
    const auto listSize = args[1]->listSize();
    const auto listElems = args[1]->listElems();

    for (unsigned int n = 0; n < listSize; ++n) {
        Value * vElem = listElems[n];
        state.forceAttrs(
            *vElem,
            noPos,
            "while evaluating a value of the list passed as second argument to "
            "builtins.zipAttrsWith"
        );
        for (auto & attr : *vElem->attrs) {
            attrsSeen[attr.name].first++;
        }
    }

    auto attrs = state.buildBindings(attrsSeen.size());
    for (auto & [sym, elem] : attrsSeen) {
        auto & list = attrs.alloc(sym);
        state.mkList(list, elem.first);
        elem.second = list.listElems();
    }
    v.mkAttrs(attrs.alreadySorted());

    for (unsigned int n = 0; n < listSize; ++n) {
        Value * vElem = listElems[n];
        for (auto & attr : *vElem->attrs) {
            *attrsSeen[attr.name].second++ = attr.value;
        }
    }

    for (auto & attr : *v.attrs) {
        auto name = state.allocValue();
        name->mkString(state.symbols[attr.name]);
        auto call1 = state.allocValue();
        call1->mkApp(args[0], name);
        auto call2 = state.allocValue();
        call2->mkApp(call1, attr.value);
        attr.value = call2;
    }
}

PrimOp primop_zipAttrsWith({
    .name = "__zipAttrsWith",
    .args = {"f", "list"},
    .doc = R"(
      Transpose a list of attribute sets into an attribute set of lists,
      then apply `mapAttrs`.

      `f` receives two arguments: the attribute name and a non-empty
      list of all values encountered for that attribute name.

      The result is an attribute set where the attribute names are the
      union of the attribute names in each element of `list`. The attribute
      values are the return values of `f`.

      ```nix
      builtins.zipAttrsWith
        (name: values: { inherit name values; })
        [ { a = "x"; } { a = "y"; b = "z"; } ]
      ```

      evaluates to

      ```
      {
        a = { name = "a"; values = [ "x" "y" ]; };
        b = { name = "b"; values = [ "z" ]; };
      }
      ```
    )",
    .fun = prim_zipAttrsWith,
});

}
