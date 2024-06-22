#include <gtest/gtest.h>

#include "eval.hh"
#include "progress-bar.hh"
#include "logging.hh"
#include "shared.hh"

constexpr std::string_view TEST_URL = "https://github.com/NixOS/nixpkgs/archive/master.tar.gz";
// Arbitrary number. We picked the size of a Nixpkgs tarball that we downloaded.
constexpr uint64_t TEST_EXPECTED = 43'370'307;
// Arbitrary number. We picked the progress made on a Nixpkgs tarball download we interrupted.
constexpr uint64_t TEST_DONE = 1'787'251;

constexpr std::string_view EXPECTED = ANSI_GREEN "1.7" ANSI_NORMAL "/41.4 MiB DL";
// Mostly here for informational purposes, but also if we change the way the escape codes
// are defined this test might break in some annoying to debug way.
constexpr std::string_view EXPECTED_RAW = "\x1b[32;1m1.7\x1b[0m/41.4 MiB DL";
static_assert(EXPECTED == EXPECTED_RAW, "Hey, hey, the ANSI escape code definitions prolly changed");

namespace nix
{
    ProgressBar & init()
    {
        initNix();
        initGC();
        startProgressBar();

        assert(dynamic_cast<ProgressBar *>(logger) != nullptr);
        return dynamic_cast<ProgressBar &>(*logger);
    }

    TEST(ProgressBar, basicStatusRender) {
        ProgressBar & progressBar = init();

        Activity act(
            progressBar,
            lvlDebug,
            actFileTransfer,
            fmt("downloading '%s'", TEST_URL),
            { "https://github.com/NixOS/nixpkgs/archive/master.tar.gz" }
        );
        act.progress(TEST_DONE, TEST_EXPECTED);
        auto state = progressBar.state_.lock();
        std::string const renderedStatus = progressBar.getStatus(*state);

        ASSERT_EQ(renderedStatus, EXPECTED);
    }

    //TEST(ProgressBar, resultPermute) {
    //    ProgressBar & progressBar = init();
    //
    //    Activity act(
    //        progressBar,
    //        lvlDebug,
    //        actBuild,
    //        "building '/nix/store/zdp9na4ci9s3g68xi9hnn5gbllf6ak03-lix-2.91.0-dev-lixpre20240616-0ba37dc.drv'",
    //        Logger::Fields{
    //            "/nix/store/zdp9na4ci9s3g68xi9hnn5gbllf6ak03-lix-2.91.0-dev-lixpre20240616-0ba37dc.drv",
    //            "",
    //            1,
    //            1,
    //        }
    //    );
    //
    //    act.progress(
    //        /* done */ 13,
    //        /* expected */ 156,
    //        /* running */ 2,
    //        /* failed */ 0
    //    );
    //
    //    act.progress(
    //        /* done */ 13,
    //        /* expected */ 156,
    //        /* running */ 2,
    //        /* failed */ 0
    //    );
    //
    //    auto state = progressBar.state_.lock();
    //    std::string const autoStatus = progressBar.getStatus(*state);
    //
    //    ASSERT_EQ(autoStatus, "a");
    //}
}
