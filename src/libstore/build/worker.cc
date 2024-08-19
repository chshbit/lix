#include "charptr-cast.hh"
#include "machines.hh"
#include "worker.hh"
#include "substitution-goal.hh"
#include "drv-output-substitution-goal.hh"
#include "local-derivation-goal.hh"
#include "signals.hh"
#include "hook-instance.hh" // IWYU pragma: keep

#include <poll.h>

namespace nix {

Worker::Worker(Store & store, Store & evalStore)
    : act(*logger, actRealise)
    , actDerivations(*logger, actBuilds)
    , actSubstitutions(*logger, actCopyPaths)
    , store(store)
    , evalStore(evalStore)
{
    /* Debugging: prevent recursive workers. */
    nrLocalBuilds = 0;
    nrSubstitutions = 0;
    lastWokenUp = steady_time_point::min();
}


Worker::~Worker()
{
    /* Explicitly get rid of all strong pointers now.  After this all
       goals that refer to this worker should be gone.  (Otherwise we
       are in trouble, since goals may call childTerminated() etc. in
       their destructors). */
    topGoals.clear();

    assert(expectedSubstitutions == 0);
    assert(expectedDownloadSize == 0);
    assert(expectedNarSize == 0);
}


std::shared_ptr<DerivationGoal> Worker::makeDerivationGoalCommon(
    const StorePath & drvPath,
    const OutputsSpec & wantedOutputs,
    std::function<std::shared_ptr<DerivationGoal>()> mkDrvGoal)
{
    std::weak_ptr<DerivationGoal> & goal_weak = derivationGoals[drvPath];
    std::shared_ptr<DerivationGoal> goal = goal_weak.lock();
    if (!goal) {
        goal = mkDrvGoal();
        goal_weak = goal;
        wakeUp(goal);
    } else {
        goal->addWantedOutputs(wantedOutputs);
    }
    return goal;
}


std::shared_ptr<DerivationGoal> Worker::makeDerivationGoal(const StorePath & drvPath,
    const OutputsSpec & wantedOutputs, BuildMode buildMode)
{
    return makeDerivationGoalCommon(drvPath, wantedOutputs, [&]() -> std::shared_ptr<DerivationGoal> {
        return !dynamic_cast<LocalStore *>(&store)
            ? std::make_shared<DerivationGoal>(drvPath, wantedOutputs, *this, buildMode)
            : LocalDerivationGoal::makeLocalDerivationGoal(drvPath, wantedOutputs, *this, buildMode);
    });
}


std::shared_ptr<DerivationGoal> Worker::makeBasicDerivationGoal(const StorePath & drvPath,
    const BasicDerivation & drv, const OutputsSpec & wantedOutputs, BuildMode buildMode)
{
    return makeDerivationGoalCommon(drvPath, wantedOutputs, [&]() -> std::shared_ptr<DerivationGoal> {
        return !dynamic_cast<LocalStore *>(&store)
            ? std::make_shared<DerivationGoal>(drvPath, drv, wantedOutputs, *this, buildMode)
            : LocalDerivationGoal::makeLocalDerivationGoal(drvPath, drv, wantedOutputs, *this, buildMode);
    });
}


std::shared_ptr<PathSubstitutionGoal> Worker::makePathSubstitutionGoal(const StorePath & path, RepairFlag repair, std::optional<ContentAddress> ca)
{
    std::weak_ptr<PathSubstitutionGoal> & goal_weak = substitutionGoals[path];
    auto goal = goal_weak.lock(); // FIXME
    if (!goal) {
        goal = std::make_shared<PathSubstitutionGoal>(path, *this, repair, ca);
        goal_weak = goal;
        wakeUp(goal);
    }
    return goal;
}


std::shared_ptr<DrvOutputSubstitutionGoal> Worker::makeDrvOutputSubstitutionGoal(const DrvOutput& id, RepairFlag repair, std::optional<ContentAddress> ca)
{
    std::weak_ptr<DrvOutputSubstitutionGoal> & goal_weak = drvOutputSubstitutionGoals[id];
    auto goal = goal_weak.lock(); // FIXME
    if (!goal) {
        goal = std::make_shared<DrvOutputSubstitutionGoal>(id, *this, repair, ca);
        goal_weak = goal;
        wakeUp(goal);
    }
    return goal;
}


GoalPtr Worker::makeGoal(const DerivedPath & req, BuildMode buildMode)
{
    return std::visit(overloaded {
        [&](const DerivedPath::Built & bfd) -> GoalPtr {
            if (auto bop = std::get_if<DerivedPath::Opaque>(&*bfd.drvPath))
                return makeDerivationGoal(bop->path, bfd.outputs, buildMode);
            else
                throw UnimplementedError("Building dynamic derivations in one shot is not yet implemented.");
        },
        [&](const DerivedPath::Opaque & bo) -> GoalPtr {
            return makePathSubstitutionGoal(bo.path, buildMode == bmRepair ? Repair : NoRepair);
        },
    }, req.raw());
}


template<typename K, typename G>
static void removeGoal(std::shared_ptr<G> goal, std::map<K, std::weak_ptr<G>> & goalMap)
{
    /* !!! inefficient */
    for (auto i = goalMap.begin();
         i != goalMap.end(); )
        if (i->second.lock() == goal) {
            auto j = i; ++j;
            goalMap.erase(i);
            i = j;
        }
        else ++i;
}


void Worker::goalFinished(GoalPtr goal, Goal::Finished & f)
{
    goal->trace("done");
    assert(!goal->exitCode.has_value());
    goal->exitCode = f.result;

    permanentFailure |= f.permanentFailure;
    timedOut |= f.timedOut;
    hashMismatch |= f.hashMismatch;
    checkMismatch |= f.checkMismatch;

    if (f.ex) {
        if (!goal->waiters.empty())
            logError(f.ex->info());
        else
            goal->ex = std::move(f.ex);
    }

    for (auto & i : goal->waiters) {
        if (GoalPtr waiting = i.lock()) {
            assert(waiting->waitees.count(goal));
            waiting->waitees.erase(goal);

            waiting->trace(fmt("waitee '%s' done; %d left", goal->name, waiting->waitees.size()));

            if (f.result != Goal::ecSuccess) ++waiting->nrFailed;
            if (f.result == Goal::ecNoSubstituters) ++waiting->nrNoSubstituters;
            if (f.result == Goal::ecIncompleteClosure) ++waiting->nrIncompleteClosure;

            if (waiting->waitees.empty() || (f.result == Goal::ecFailed && !settings.keepGoing)) {
                /* If we failed and keepGoing is not set, we remove all
                   remaining waitees. */
                for (auto & i : waiting->waitees) {
                    i->waiters.extract(waiting);
                }
                waiting->waitees.clear();

                wakeUp(waiting);
            }

            waiting->waiteeDone(goal);
        }
    }
    goal->waiters.clear();
    removeGoal(goal);
    goal->cleanup();
}

void Worker::handleWorkResult(GoalPtr goal, Goal::WorkResult how)
{
    std::visit(
        overloaded{
            [&](Goal::StillAlive) {},
            [&](Goal::WaitForSlot) { waitForBuildSlot(goal); },
            [&](Goal::WaitForAWhile) { waitForAWhile(goal); },
            [&](Goal::ContinueImmediately) { wakeUp(goal); },
            [&](Goal::WaitForGoals & w) {
                for (auto & dep : w.goals) {
                    goal->waitees.insert(dep);
                    dep->waiters.insert(goal);
                }
            },
            [&](Goal::Finished & f) { goalFinished(goal, f); },
        },
        how
    );
}

void Worker::removeGoal(GoalPtr goal)
{
    if (auto drvGoal = std::dynamic_pointer_cast<DerivationGoal>(goal))
        nix::removeGoal(drvGoal, derivationGoals);
    else if (auto subGoal = std::dynamic_pointer_cast<PathSubstitutionGoal>(goal))
        nix::removeGoal(subGoal, substitutionGoals);
    else if (auto subGoal = std::dynamic_pointer_cast<DrvOutputSubstitutionGoal>(goal))
        nix::removeGoal(subGoal, drvOutputSubstitutionGoals);
    else
        assert(false);

    if (topGoals.find(goal) != topGoals.end()) {
        topGoals.erase(goal);
        /* If a top-level goal failed, then kill all other goals
           (unless keepGoing was set). */
        if (goal->exitCode == Goal::ecFailed && !settings.keepGoing)
            topGoals.clear();
    }
}


void Worker::wakeUp(GoalPtr goal)
{
    goal->trace("woken up");
    awake.insert(goal);
}


void Worker::childStarted(GoalPtr goal, const std::set<int> & fds,
    bool inBuildSlot)
{
    Child child;
    child.goal = goal;
    child.goal2 = goal.get();
    child.fds = fds;
    child.timeStarted = child.lastOutput = steady_time_point::clock::now();
    child.inBuildSlot = inBuildSlot;
    children.emplace_back(child);
    if (inBuildSlot) {
        switch (goal->jobCategory()) {
        case JobCategory::Substitution:
            nrSubstitutions++;
            break;
        case JobCategory::Build:
            nrLocalBuilds++;
            break;
        default:
            abort();
        }
    }
}


void Worker::childTerminated(Goal * goal)
{
    auto i = std::find_if(children.begin(), children.end(),
        [&](const Child & child) { return child.goal2 == goal; });
    if (i == children.end()) return;

    if (i->inBuildSlot) {
        switch (goal->jobCategory()) {
        case JobCategory::Substitution:
            assert(nrSubstitutions > 0);
            nrSubstitutions--;
            break;
        case JobCategory::Build:
            assert(nrLocalBuilds > 0);
            nrLocalBuilds--;
            break;
        default:
            abort();
        }
    }

    children.erase(i);

    /* Wake up goals waiting for a build slot. */
    for (auto & j : wantingToBuild) {
        GoalPtr goal = j.lock();
        if (goal) wakeUp(goal);
    }

    wantingToBuild.clear();
}


void Worker::waitForBuildSlot(GoalPtr goal)
{
    goal->trace("wait for build slot");
    bool isSubstitutionGoal = goal->jobCategory() == JobCategory::Substitution;
    if ((!isSubstitutionGoal && nrLocalBuilds < settings.maxBuildJobs) ||
        (isSubstitutionGoal && nrSubstitutions < settings.maxSubstitutionJobs))
        wakeUp(goal); /* we can do it right away */
    else
        wantingToBuild.insert(goal);
}


void Worker::waitForAWhile(GoalPtr goal)
{
    debug("wait for a while");
    waitingForAWhile.insert(goal);
}


void Worker::run(const Goals & _topGoals)
{
    std::vector<nix::DerivedPath> topPaths;

    for (auto & i : _topGoals) {
        topGoals.insert(i);
        if (auto goal = dynamic_cast<DerivationGoal *>(i.get())) {
            topPaths.push_back(DerivedPath::Built {
                .drvPath = makeConstantStorePathRef(goal->drvPath),
                .outputs = goal->wantedOutputs,
            });
        } else if (auto goal = dynamic_cast<PathSubstitutionGoal *>(i.get())) {
            topPaths.push_back(DerivedPath::Opaque{goal->storePath});
        }
    }

    /* Call queryMissing() to efficiently query substitutes. */
    StorePathSet willBuild, willSubstitute, unknown;
    uint64_t downloadSize, narSize;
    store.queryMissing(topPaths, willBuild, willSubstitute, unknown, downloadSize, narSize);

    debug("entered goal loop");

    while (1) {

        checkInterrupt();

        // TODO GC interface?
        if (auto localStore = dynamic_cast<LocalStore *>(&store))
            localStore->autoGC(false);

        /* Call every wake goal (in the ordering established by
           CompareGoalPtrs). */
        while (!awake.empty() && !topGoals.empty()) {
            Goals awake2;
            for (auto & i : awake) {
                GoalPtr goal = i.lock();
                if (goal) awake2.insert(goal);
            }
            awake.clear();
            for (auto & goal : awake2) {
                checkInterrupt();
                /* Make sure that we are always allowed to run at least one substitution.
                   This prevents infinite waiting. */
                const bool inSlot = goal->jobCategory() == JobCategory::Substitution
                    ? nrSubstitutions < std::max(1U, (unsigned int) settings.maxSubstitutionJobs)
                    : nrLocalBuilds < settings.maxBuildJobs;
                handleWorkResult(goal, goal->work(inSlot));

                actDerivations.progress(
                    doneBuilds, expectedBuilds + doneBuilds, runningBuilds, failedBuilds
                );
                actSubstitutions.progress(
                    doneSubstitutions,
                    expectedSubstitutions + doneSubstitutions,
                    runningSubstitutions,
                    failedSubstitutions
                );
                act.setExpected(actFileTransfer, expectedDownloadSize + doneDownloadSize);
                act.setExpected(actCopyPath, expectedNarSize + doneNarSize);

                if (topGoals.empty()) break; // stuff may have been cancelled
            }
        }

        if (topGoals.empty()) break;

        /* Wait for input. */
        if (!children.empty() || !waitingForAWhile.empty())
            waitForInput();
        else {
            if (awake.empty() && 0U == settings.maxBuildJobs)
            {
                if (getMachines().empty())
                   throw Error("unable to start any build; either increase '--max-jobs' "
                            "or enable remote builds."
                            "\nhttps://docs.lix.systems/manual/lix/stable/advanced-topics/distributed-builds.html");
                else
                   throw Error("unable to start any build; remote machines may not have "
                            "all required system features."
                            "\nhttps://docs.lix.systems/manual/lix/stable/advanced-topics/distributed-builds.html");

            }
            assert(!awake.empty());
        }
    }

    /* If --keep-going is not set, it's possible that the main goal
       exited while some of its subgoals were still active.  But if
       --keep-going *is* set, then they must all be finished now. */
    assert(!settings.keepGoing || awake.empty());
    assert(!settings.keepGoing || wantingToBuild.empty());
    assert(!settings.keepGoing || children.empty());
}

void Worker::waitForInput()
{
    printMsg(lvlVomit, "waiting for children");

    /* Process output from the file descriptors attached to the
       children, namely log output and output path creation commands.
       We also use this to detect child termination: if we get EOF on
       the logger pipe of a build, we assume that the builder has
       terminated. */

    bool useTimeout = false;
    long timeout = 0;
    auto before = steady_time_point::clock::now();

    /* If we're monitoring for silence on stdout/stderr, or if there
       is a build timeout, then wait for input until the first
       deadline for any child. */
    auto nearest = steady_time_point::max(); // nearest deadline
    if (settings.minFree.get() != 0)
        // Periodicallty wake up to see if we need to run the garbage collector.
        nearest = before + std::chrono::seconds(10);
    for (auto & i : children) {
        if (auto goal = i.goal.lock()) {
            if (!goal->respectsTimeouts()) continue;
            if (0 != settings.maxSilentTime)
                nearest = std::min(nearest, i.lastOutput + std::chrono::seconds(settings.maxSilentTime));
            if (0 != settings.buildTimeout)
                nearest = std::min(nearest, i.timeStarted + std::chrono::seconds(settings.buildTimeout));
        }
    }
    if (nearest != steady_time_point::max()) {
        timeout = std::max(1L, (long) std::chrono::duration_cast<std::chrono::seconds>(nearest - before).count());
        useTimeout = true;
    }

    /* If we are polling goals that are waiting for a lock, then wake
       up after a few seconds at most. */
    if (!waitingForAWhile.empty()) {
        useTimeout = true;
        if (lastWokenUp == steady_time_point::min() || lastWokenUp > before) lastWokenUp = before;
        timeout = std::max(1L,
            (long) std::chrono::duration_cast<std::chrono::seconds>(
                lastWokenUp + std::chrono::seconds(settings.pollInterval) - before).count());
    } else lastWokenUp = steady_time_point::min();

    if (useTimeout)
        vomit("sleeping %d seconds", timeout);

    /* Use select() to wait for the input side of any logger pipe to
       become `available'.  Note that `available' (i.e., non-blocking)
       includes EOF. */
    std::vector<struct pollfd> pollStatus;
    std::map<int, size_t> fdToPollStatus;
    for (auto & i : children) {
        for (auto & j : i.fds) {
            pollStatus.push_back((struct pollfd) { .fd = j, .events = POLLIN });
            fdToPollStatus[j] = pollStatus.size() - 1;
        }
    }

    if (poll(pollStatus.data(), pollStatus.size(),
            useTimeout ? timeout * 1000 : -1) == -1) {
        if (errno == EINTR) return;
        throw SysError("waiting for input");
    }

    auto after = steady_time_point::clock::now();

    /* Process all available file descriptors. FIXME: this is
       O(children * fds). */
    decltype(children)::iterator i;
    for (auto j = children.begin(); j != children.end(); j = i) {
        i = std::next(j);

        checkInterrupt();

        GoalPtr goal = j->goal.lock();
        assert(goal);

        if (!goal->exitCode.has_value() &&
            0 != settings.maxSilentTime &&
            goal->respectsTimeouts() &&
            after - j->lastOutput >= std::chrono::seconds(settings.maxSilentTime))
        {
            handleWorkResult(
                goal,
                goal->timedOut(Error(
                    "%1% timed out after %2% seconds of silence",
                    goal->getName(),
                    settings.maxSilentTime
                ))
            );
            continue;
        }

        else if (!goal->exitCode.has_value() &&
            0 != settings.buildTimeout &&
            goal->respectsTimeouts() &&
            after - j->timeStarted >= std::chrono::seconds(settings.buildTimeout))
        {
            handleWorkResult(
                goal,
                goal->timedOut(
                    Error("%1% timed out after %2% seconds", goal->getName(), settings.buildTimeout)
                )
            );
            continue;
        }

        std::set<int> fds2(j->fds);
        std::vector<unsigned char> buffer(4096);
        for (auto & k : fds2) {
            const auto fdPollStatusId = get(fdToPollStatus, k);
            assert(fdPollStatusId);
            assert(*fdPollStatusId < pollStatus.size());
            if (pollStatus.at(*fdPollStatusId).revents) {
                ssize_t rd = ::read(k, buffer.data(), buffer.size());
                // FIXME: is there a cleaner way to handle pt close
                // than EIO? Is this even standard?
                if (rd == 0 || (rd == -1 && errno == EIO)) {
                    debug("%1%: got EOF", goal->getName());
                    goal->handleEOF(k);
                    handleWorkResult(goal, Goal::ContinueImmediately{});
                    j->fds.erase(k);
                } else if (rd == -1) {
                    if (errno != EINTR)
                        throw SysError("%s: read failed", goal->getName());
                } else {
                    printMsg(lvlVomit, "%1%: read %2% bytes",
                        goal->getName(), rd);
                    std::string_view data(charptr_cast<char *>(buffer.data()), rd);
                    j->lastOutput = after;
                    handleWorkResult(goal, goal->handleChildOutput(k, data));
                }
            }
        }
    }

    if (!waitingForAWhile.empty() && lastWokenUp + std::chrono::seconds(settings.pollInterval) <= after) {
        lastWokenUp = after;
        for (auto & i : waitingForAWhile) {
            GoalPtr goal = i.lock();
            if (goal) wakeUp(goal);
        }
        waitingForAWhile.clear();
    }
}


unsigned int Worker::failingExitStatus()
{
    // See API docs in header for explanation
    unsigned int mask = 0;
    bool buildFailure = permanentFailure || timedOut || hashMismatch;
    if (buildFailure)
        mask |= 0x04;  // 100
    if (timedOut)
        mask |= 0x01;  // 101
    if (hashMismatch)
        mask |= 0x02;  // 102
    if (checkMismatch) {
        mask |= 0x08;  // 104
    }

    if (mask)
        mask |= 0x60;
    return mask ? mask : 1;
}


bool Worker::pathContentsGood(const StorePath & path)
{
    auto i = pathContentsGoodCache.find(path);
    if (i != pathContentsGoodCache.end()) return i->second;
    printInfo("checking path '%s'...", store.printStorePath(path));
    auto info = store.queryPathInfo(path);
    bool res;
    if (!pathExists(store.printStorePath(path)))
        res = false;
    else {
        HashResult current = hashPath(info->narHash.type, store.printStorePath(path));
        Hash nullHash(HashType::SHA256);
        res = info->narHash == nullHash || info->narHash == current.first;
    }
    pathContentsGoodCache.insert_or_assign(path, res);
    if (!res)
        printError("path '%s' is corrupted or missing!", store.printStorePath(path));
    return res;
}


void Worker::markContentsGood(const StorePath & path)
{
    pathContentsGoodCache.insert_or_assign(path, true);
}

}
