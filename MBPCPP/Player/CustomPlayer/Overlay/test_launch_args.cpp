#include <cassert>
#include <string>
#include "launch_args.hpp"

int main() {
    std::wstring uri;
    assert(ParseLaunchUri(L"--uri \"https://test.com/video.mp4\"", uri));
    assert(uri == L"https://test.com/video.mp4");
    assert(ParseLaunchUri(L"--foo x --uri \"C:\\Videos\\Test Movie.mp4\" --bar", uri));
    assert(uri == L"C:\\Videos\\Test Movie.mp4");
    assert(ParseLaunchUri(L"--uri=https://example.com/a.mp4?x=1&y=2", uri));
    assert(uri == L"https://example.com/a.mp4?x=1&y=2");
    assert(!ParseLaunchUri(L"--foo bar", uri));

    LaunchOptions bridge = ParseLaunchOptions(L"--bridge \"MovieBoxPlayerMod-1-abc\" --user-agent \"MovieBox UA/1.0\" --VPQ True");
    assert(bridge.hasBridge);
    assert(bridge.bridgeName == L"MovieBoxPlayerMod-1-abc");
    assert(bridge.hasUserAgent);
    assert(bridge.userAgent == L"MovieBox UA/1.0");
    assert(bridge.hasVPQ && bridge.videoPlayerQuits);

    LaunchOptions equals = ParseLaunchOptions(L"--bridge=pipe-x --user-agent=UA --uri=https://example/video.m3u8");
    assert(equals.hasBridge && equals.bridgeName == L"pipe-x");
    assert(equals.hasUserAgent && equals.userAgent == L"UA");
    assert(equals.hasUri && equals.uri == L"https://example/video.m3u8");
    return 0;
}
