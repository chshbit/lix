#include "progress-bar.hh"
#include "sync.hh"
#include "store-api.hh"
#include "names.hh"
#include "terminal.hh"

#include <atomic>
#include <map>
#include <thread>
#include <sstream>
#include <iostream>
#include <chrono>

#include <boost/container/static_vector.hpp>

namespace nix {

using namespace std::literals::chrono_literals;

static std::string_view getS(const std::vector<Logger::Field> & fields, size_t n)
{
    assert(n < fields.size());
    assert(fields[n].type == Logger::Field::tString);
    return fields[n].s;
}

static uint64_t getI(const std::vector<Logger::Field> & fields, size_t n)
{
    assert(n < fields.size());
    assert(fields[n].type == Logger::Field::tInt);
    return fields[n].i;
}

static std::string_view storePathToName(std::string_view path)
{
    auto base = baseNameOf(path);
    auto i = base.find('-');
    return i == std::string::npos ? base.substr(0, 0) : base.substr(i + 1);
}


ProgressBar::~ProgressBar()
{
    stop();
}

/* Called by destructor, can't be overridden */
void ProgressBar::stop()
{
    {
        auto state(state_.lock());
        if (!state->active) return;
        state->active = false;
        writeToStderr("\r\e[K");
        updateCV.notify_one();
        quitCV.notify_one();
    }
    updateThread.join();
}

void ProgressBar::pause()
{
    state_.lock()->paused = true;
    writeToStderr("\r\e[K");
}

void ProgressBar::resume()
{
    state_.lock()->paused = false;
    writeToStderr("\r\e[K");
    state_.lock()->haveUpdate = true;
    updateCV.notify_one();
}

bool ProgressBar::isVerbose()
{
    return printBuildLogs;
}

void ProgressBar::log(Verbosity lvl, std::string_view s)
{
    if (lvl > verbosity) return;
    auto state(state_.lock());
    log(*state, lvl, s);
}

void ProgressBar::logEI(const ErrorInfo & ei)
{
    auto state(state_.lock());

    std::stringstream oss;
    showErrorInfo(oss, ei, loggerSettings.showTrace.get());

    log(*state, ei.level, oss.str());
}

void ProgressBar::log(State & state, Verbosity lvl, std::string_view s)
{
    if (state.active) {
        writeToStderr("\r\e[K" + filterANSIEscapes(s, !isTTY) + ANSI_NORMAL "\n");
        draw(state);
    } else {
        auto s2 = s + ANSI_NORMAL "\n";
        if (!isTTY) s2 = filterANSIEscapes(s2, true);
        writeToStderr(s2);
    }
}

void ProgressBar::startActivity(
    ActivityId act,
    Verbosity lvl,
    ActivityType type,
    const std::string & s,
    const Fields & fields,
    ActivityId parent
)
{
    auto state(state_.lock());

    if (lvl <= verbosity && !s.empty() && type != actBuildWaiting)
        log(*state, lvl, s + "...");

    state->activities.emplace_back(ActInfo {
        .s = s,
        .type = type,
        .parent = parent,
        .startTime = std::chrono::steady_clock::now()
    });

    auto mostRecentAct = std::prev(state->activities.end());
    state->its.emplace(act, mostRecentAct);
    state->activitiesByType[type].its.emplace(act, mostRecentAct);

    if (type == actBuild) {
        std::string name(storePathToName(getS(fields, 0)));
        if (name.ends_with(".drv"))
            name = name.substr(0, name.size() - 4);
        mostRecentAct->s = fmt("building " ANSI_BOLD "%s" ANSI_NORMAL, name);
        auto machineName = getS(fields, 1);
        if (machineName != "")
            mostRecentAct->s += fmt(" on " ANSI_BOLD "%s" ANSI_NORMAL, machineName);

        // Used to be curRound and nrRounds, but the
        // implementation was broken for a long time.
        if (getI(fields, 2) != 1 || getI(fields, 3) != 1) {
            throw Error("log message indicated repeating builds, but this is not currently implemented");
        }
        mostRecentAct->name = DrvName(name).name;
    }

    if (type == actSubstitute) {
        auto name = storePathToName(getS(fields, 0));
        auto sub = getS(fields, 1);
        mostRecentAct->s = fmt(
            sub.starts_with("local")
            ? "copying " ANSI_BOLD "%s" ANSI_NORMAL " from %s"
            : "fetching " ANSI_BOLD "%s" ANSI_NORMAL " from %s",
            name, sub);
    }

    if (type == actPostBuildHook) {
        auto name = storePathToName(getS(fields, 0));
        if (name.ends_with(".drv"))
            name = name.substr(0, name.size() - 4);
        mostRecentAct->s = fmt("post-build " ANSI_BOLD "%s" ANSI_NORMAL, name);
        mostRecentAct->name = DrvName(name).name;
    }

    if (type == actQueryPathInfo) {
        auto name = storePathToName(getS(fields, 0));
        mostRecentAct->s = fmt("querying " ANSI_BOLD "%s" ANSI_NORMAL " on %s", name, getS(fields, 1));
    }

    if (ancestorTakesPrecedence(*state, type, parent)) {
        mostRecentAct->visible = false;
    }

    update(*state);
}

bool ProgressBar::hasAncestor(State & state, ActivityType type, ActivityId act) const
{
    // 0 is the sentinel value for a non-existent activity ID.
    while (act != 0) {
        auto const foundActIt = state.its.find(act);
        if (foundActIt == state.its.end()) {
            break;
        }

        ActInfo const & foundActInfo = *foundActIt->second;

        if (foundActInfo.type == type) {
            return true;
        }

        act = foundActInfo.parent;
    }
    return false;
}

void ProgressBar::stopActivity(ActivityId act)
{
    auto state(state_.lock());

    auto const currentAct = state->its.find(act);
    if (currentAct != state->its.end()) {

        std::list<ActInfo>::iterator const & actInfo = currentAct->second;

        auto & actByType = state->activitiesByType[actInfo->type];
        actByType.done += actInfo->done;
        actByType.failed += actInfo->failed;

        for (auto const & [type, expected] : actInfo->expectedByType) {
            state->activitiesByType[type].expected -= expected;
        }

        actByType.its.erase(act);
        state->activities.erase(actInfo);
        state->its.erase(currentAct);
    }

    update(*state);
}

void ProgressBar::result(ActivityId act, ResultType type, const std::vector<Field> & fields)
{
    auto state(state_.lock());

    if (type == resFileLinked) {
        state->filesLinked++;
        state->bytesLinked += getI(fields, 0);
        update(*state);
    }

    else if (type == resBuildLogLine || type == resPostBuildLogLine) {
        auto lastLine = chomp(getS(fields, 0));
        if (!lastLine.empty()) {
            auto found = state->its.find(act);
            assert(found != state->its.end());
            ActInfo info = *found->second;
            if (printBuildLogs) {
                auto suffix = "> ";
                if (type == resPostBuildLogLine) {
                    suffix = " (post)> ";
                }
                log(*state, lvlInfo, ANSI_FAINT + info.name.value_or("unnamed") + suffix + ANSI_NORMAL + lastLine);
            } else {
                state->activities.erase(found->second);
                info.lastLine = lastLine;
                state->activities.emplace_back(info);
                found->second = std::prev(state->activities.end());
                update(*state);
            }
        }
    }

    else if (type == resUntrustedPath) {
        state->untrustedPaths++;
        update(*state);
    }

    else if (type == resCorruptedPath) {
        state->corruptedPaths++;
        update(*state);
    }

    else if (type == resSetPhase) {
        auto i = state->its.find(act);
        assert(i != state->its.end());
        i->second->phase = getS(fields, 0);
        update(*state);
    }

    else if (type == resProgress) {
        auto i = state->its.find(act);
        assert(i != state->its.end());
        ActInfo & actInfo = *i->second;
        actInfo.done = getI(fields, 0);
        actInfo.expected = getI(fields, 1);
        actInfo.running = getI(fields, 2);
        actInfo.failed = getI(fields, 3);
        update(*state);
    }

    else if (type == resSetExpected) {
        auto i = state->its.find(act);
        assert(i != state->its.end());
        ActInfo & actInfo = *i->second;
        auto const type = static_cast<ActivityType>(getI(fields, 0));
        uint64_t & thisExpected = actInfo.expectedByType[type];
        state->activitiesByType[type].expected -= thisExpected;
        thisExpected = getI(fields, 1);
        state->activitiesByType[type].expected += thisExpected;
        update(*state);
    }
}

void ProgressBar::update(State & state)
{
    state.haveUpdate = true;
    updateCV.notify_one();
}

std::chrono::milliseconds ProgressBar::draw(State & state)
{
    auto nextWakeup = A_LONG_TIME;

    state.haveUpdate = false;
    if (state.paused || !state.active) return nextWakeup;

    std::string line;

    std::string status = getStatus(state);
    if (!status.empty()) {
        line += '[';
        line += status;
        line += "]";
    }

    auto now = std::chrono::steady_clock::now();

    if (!state.activities.empty()) {
        if (!status.empty()) line += " ";
        auto i = state.activities.rbegin();

        while (i != state.activities.rend()) {
            if (i->visible && (!i->s.empty() || !i->lastLine.empty())) {
                /* Don't show activities until some time has
                   passed, to avoid displaying very short
                   activities. */
                auto delay = std::chrono::milliseconds(10);
                if (i->startTime + delay < now)
                    break;
                else
                    nextWakeup = std::min(nextWakeup, std::chrono::duration_cast<std::chrono::milliseconds>(delay - (now - i->startTime)));
            }
            ++i;
        }

        if (i != state.activities.rend()) {
            line += i->s;
            if (!i->phase.empty()) {
                line += " (";
                line += i->phase;
                line += ")";
            }
            if (!i->lastLine.empty()) {
                if (!i->s.empty()) line += ": ";
                line += i->lastLine;
            }
        }
    }

    auto width = getWindowSize().second;
    if (width <= 0) width = std::numeric_limits<decltype(width)>::max();

    writeToStderr("\r" + filterANSIEscapes(line, false, width) + ANSI_NORMAL + "\e[K");

    return nextWakeup;
}

std::string ProgressBar::getStatus(State & state) const
{
    constexpr static auto MiB = 1024.0 * 1024.0;

    // Each time the `showActivity` lambda is called:
    // actBuild, actFileTransfer, actVerifyPaths,
    // + 3 permutations for actCopyPaths, actCopyPath,
    // + corrupted and unstrusted paths.
    constexpr static auto MAX_PARTS = 8;

    // Stack-allocated vector.
    // No need to heap allocate here when we have such a low maxiumum.
    boost::container::static_vector<std::string, MAX_PARTS> parts;

    auto renderActivity = [&](ActivityType type, const std::string & itemFmt, const std::string & numberFmt = "%d", double unit = 1) {
        auto const & act = state.activitiesByType[type];
        uint64_t done = act.done, expected = act.done, running = 0, failed = act.failed;
        for (auto const & [actId, infoIt] : act.its) {
            done += infoIt->done;
            expected += infoIt->expected;
            running += infoIt->running;
            failed += infoIt->failed;
        }

        expected = std::max(expected, act.expected);

        std::string rendered;

        if (running || done || expected || failed) {
            if (running) {
                if (expected != 0) {
                    auto const runningPart = fmt(numberFmt, running / unit);
                    auto const donePart = fmt(numberFmt, done / unit);
                    auto const expectedPart = fmt(numberFmt, expected / unit);
                    rendered = fmt(
                        ANSI_BLUE "%s" ANSI_NORMAL "/" ANSI_GREEN "%s" ANSI_NORMAL "/%s",
                        runningPart,
                        donePart,
                        expectedPart
                    );
                } else {
                    auto const runningPart = fmt(numberFmt, running / unit);
                    auto const donePart = fmt(numberFmt, done / unit);
                    rendered = fmt(
                        ANSI_BLUE "%s" ANSI_NORMAL "/" ANSI_GREEN "%s" ANSI_NORMAL,
                        runningPart,
                        donePart
                    );
                }
            } else if (expected != done) {
                if (expected != 0) {
                    auto const donePart = fmt(numberFmt, done / unit);
                    auto const expectedPart = fmt(numberFmt, expected / unit);
                    rendered = fmt(
                        ANSI_GREEN "%s" ANSI_NORMAL "/%s",
                        donePart,
                        expectedPart
                    );
                } else {
                    auto const donePart = fmt(numberFmt, done / unit);
                    rendered = fmt(ANSI_GREEN "%s" ANSI_NORMAL, donePart);
                }
            } else {
                auto const donePart = fmt(numberFmt, done / unit);

                // We only color if `done` is non-zero.
                if (done) {
                    rendered = concatStrings(ANSI_GREEN, donePart, ANSI_NORMAL);
                } else {
                    rendered = donePart;
                }
            }
            rendered = fmt(itemFmt, rendered);

            if (failed)
                rendered += fmt(" (" ANSI_RED "%d failed" ANSI_NORMAL ")", failed / unit);
        }

        return rendered;
    };

    auto showActivity = [&](ActivityType type, const std::string & itemFmt, const std::string & numberFmt = "%d", double unit = 1) {
        auto rendered = renderActivity(type, itemFmt, numberFmt, unit);
        if (!rendered.empty()) {
            parts.emplace_back(std::move(rendered));
        }
    };

    showActivity(actBuilds, "%s built");

    {
        auto const renderedMultiCopy = renderActivity(actCopyPaths, "%s copied");
        auto const renderedSingleCopy = renderActivity(actCopyPath, "%s MiB", "%.1f", MiB);

        if (!renderedMultiCopy.empty() || !renderedSingleCopy.empty()) {
            std::string part = concatStrings(
                renderedMultiCopy.empty() ? "0 copied" : renderedMultiCopy,
                !renderedSingleCopy.empty() ? fmt(" (%s)", renderedSingleCopy) : ""
            );
            parts.emplace_back(std::move(part));
        }
    }

    showActivity(actFileTransfer, "%s MiB DL", "%.1f", MiB);

    {
        auto renderedOptimise = renderActivity(actOptimiseStore, "%s paths optimised");
        if (!renderedOptimise.empty()) {
            parts.emplace_back(std::move(renderedOptimise));
            parts.emplace_back(fmt(
                "%.1f MiB / %d inodes freed",
                state.bytesLinked / MiB,
                state.filesLinked
            ));
        }
    }

    // FIXME: don't show "done" paths in green.
    showActivity(actVerifyPaths, "%s paths verified");

    if (state.corruptedPaths) {
        parts.emplace_back(fmt(ANSI_RED "%d corrupted" ANSI_NORMAL, state.corruptedPaths));
    }

    if (state.untrustedPaths) {
        parts.emplace_back(fmt(ANSI_RED "%d untrusted" ANSI_NORMAL, state.untrustedPaths));
    }

    return concatStringsSep(", ", parts);
}

void ProgressBar::writeToStdout(std::string_view s)
{
    auto state(state_.lock());
    if (state->active) {
        std::cerr << "\r\e[K";
        Logger::writeToStdout(s);
        draw(*state);
    } else {
        Logger::writeToStdout(s);
    }
}

std::optional<char> ProgressBar::ask(std::string_view msg)
{
    auto state(state_.lock());
    if (!state->active || !isatty(STDIN_FILENO)) return {};
    std::cerr << fmt("\r\e[K%s ", msg);
    auto s = trim(readLine(STDIN_FILENO));
    if (s.size() != 1) return {};
    draw(*state);
    return s[0];
}

void ProgressBar::setPrintBuildLogs(bool printBuildLogs)
{
    this->printBuildLogs = printBuildLogs;
}

bool ProgressBar::ancestorTakesPrecedence(State & state, ActivityType type, ActivityId parent)
{
    if (type == actFileTransfer && hasAncestor(state, actCopyPath, parent)) {
        return true;
    }

    if (type == actFileTransfer && hasAncestor(state, actQueryPathInfo, parent)) {
        return true;
    }

    if (type == actCopyPath && hasAncestor(state, actSubstitute, parent)) {
        return true;
    }

    return false;
}

Logger * makeProgressBar()
{
    return new ProgressBar(shouldANSI());
}

void startProgressBar()
{
    logger = makeProgressBar();
}

void stopProgressBar()
{
    auto progressBar = dynamic_cast<ProgressBar *>(logger);
    if (progressBar) progressBar->stop();

}

}
