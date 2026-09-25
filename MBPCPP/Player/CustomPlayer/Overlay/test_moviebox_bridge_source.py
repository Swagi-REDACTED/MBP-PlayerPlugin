from pathlib import Path

root = Path(__file__).resolve().parent
launch = (root / 'launch_args.hpp').read_text(encoding='utf-8-sig')
bridge_path = root / 'moviebox_bridge.hpp'
bridge = bridge_path.read_text(encoding='utf-8-sig') if bridge_path.exists() else ''
main = (root / 'main.cpp').read_text(encoding='utf-8-sig')
video = (root / 'videoengine.cpp').read_text(encoding='utf-8-sig')

checks = []
def check(name, cond): checks.append((name, bool(cond)))

# Task 2 transport / launch contract.
check('launch options expose bridge name', 'bridgeName' in launch and 'hasBridge' in launch)
check('launch options expose user agent', 'userAgent' in launch and 'hasUserAgent' in launch)
check('bridge uses Win32 named pipe path', '\\\\\\\\.\\\\pipe\\\\' in bridge)
check('bridge waits for named pipe', 'WaitNamedPipeW' in bridge)
check('bridge opens duplex pipe', 'CreateFileW' in bridge and 'GENERIC_READ | GENERIC_WRITE' in bridge)
check('bridge reads replies', 'ReadFile(' in bridge)
check('bridge writes requests', 'WriteFile(' in bridge)
check('bridge owns worker thread', 'std::thread' in bridge and 'WorkerMain' in bridge)
check('bridge requests metadata periodically', 'metadata' in bridge and 'std::chrono::seconds(2)' in bridge)
check('bridge shutdown is bounded', 'MovieBoxBridgeStop' in bridge and 'CancelSynchronousIo' in bridge)

# Task 3 lifecycle / synchronization contract.
check('bridge implementation is instantiated once in main TU', '#define MOVIEBOX_BRIDGE_IMPLEMENTATION' in main and '#include "moviebox_bridge.hpp"' in main)
check('bridge launch starts in player mode', 'options.hasBridge' in main and 'g_AppMode = MODE_PLAYER' in main and 'MovieBoxBridgeStart' in main)
check('bridge mode bypasses standalone app routing', 'if (g_Settings.bridgeMode) {\n        RenderVideoEngine(hwnd);' in main)
check('queued dashboard exit closes bridge session instead', 's_pendingExitToDashboard && g_Settings.bridgeMode' in video)
check('render loop pumps MovieBox replies', 'PumpMovieBoxBridge' in video)
check('new source revision hot-swaps URI and title', 'reply.source' in video and 'bridgeRevision' in video and 'g_Settings.currentUrl' in video and 'g_Settings.videoTitle' in video)
source_swap = video[video.find('if (reply.source'):video.find('if (reply.metadata)')]
check('new source clears stale metadata and subtitle cues', 's_bridgeCues.clear();' in source_swap and 's_bridgeMetadataValid = false;' in source_swap)
check('resume seconds are deferred until duration exists', 'bridgeResumeSeconds' in video and 's_bridgeResumePending' in video and '/ engineDuration' in video)
check('bridge queues opened notification', 'MovieBoxBridgeQueueAction("opened")' in video)
check('bridge queues failed notification', 'MovieBoxBridgeQueueAction("failed")' in video)
check('bridge queues ended notification', 'MovieBoxBridgeQueueAction("ended")' in video)
check('bridge reports playback snapshots', 'MovieBoxBridgeSetPlaybackState' in video and 'MovieBoxPlaybackState' in video)
check('MovieBox commands are applied on render thread', 'ApplyMovieBoxCommand' in video and 'command.action' in video)
check('shutdown sends close before bridge stop', 'MovieBoxBridgeCloseSession(1200);\n        MovieBoxBridgeStop();' in main)

# Task 4 metadata-driven UI contract.
check('bridge episode choices send metadata ids', 'RenderBridgeEpisodePanel' in video and 'MovieBoxBridgeQueueAction("episode", choice.id)' in video)
check('bridge next action is exposed', 'MovieBoxBridgeQueueAction("next")' in video)
check('bridge quality choices send metadata ids', 'MovieBoxBridgeQueueAction("quality", choice.id)' in video)
check('bridge server choices send metadata ids', 'MovieBoxBridgeQueueAction("server", choice.id)' in video)
check('bridge subtitle choices send metadata ids', 'MovieBoxBridgeQueueAction("subtitle", choice.id)' in video)
check('bridge subtitle delay is adjustable', 'MovieBoxBridgeQueueAction("subtitleDelay"' in video)
check('bridge subtitles render independently of controls', 'RenderMovieBoxSubtitles' in video and video.find('RenderMovieBoxSubtitles') < video.find('s_controlsAlpha <= 0.001f'))
check('bridge episode UI is not gated on mock media', 'bridgeEpisodePanelAvailable' in video)
check('bridge episode panel fade target accepts bridge metadata',
      's_showEpPanel && !g_IsPIPMode &&\n        (bridgeEpisodePanelAvailable ||' in video)
check('bridge season selector is built from MovieBox episode groups',
      'bridgeSeasonGroups' in video and 'choice.group' in video and 's_bridgeSeasonGroup' in video)
check('bridge season selector can open',
      '##BridgeSeasonCombo' in video and 's_seasonDropdownOpen = !s_seasonDropdownOpen' in video)
check('bridge episode list filters to the selected season',
      'if (!s_bridgeSeasonGroup.empty() && choice.group != s_bridgeSeasonGroup) continue;' in video)
check('bridge season popup is rendered', 'BridgeSeasonListPopup' in video)

# Later tasks add lifecycle/UI assertions here. They intentionally do not gate Task 2 yet.

failed = [name for name, ok in checks if not ok]
for name, ok in checks:
    print(('PASS' if ok else 'FAIL') + ': ' + name)
if failed:
    raise SystemExit(f'{len(failed)} bridge source checks failed')
