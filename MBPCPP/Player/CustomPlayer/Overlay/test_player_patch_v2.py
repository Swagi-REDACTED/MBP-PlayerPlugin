
from pathlib import Path

root = Path(__file__).resolve().parent
main = (root / 'main.cpp').read_text(encoding='utf-8-sig')
cplayer = (root / 'Cplayer.cpp').read_text(encoding='utf-8-sig')
video = (root / 'videoengine.cpp').read_text(encoding='utf-8-sig')
hpp = (root / 'videoengine.hpp').read_text(encoding='utf-8-sig')

checks = []

def check(name, cond):
    checks.append((name, bool(cond)))

check('launch parser header is wired in', '#include "launch_args.hpp"' in main)
check('--uri switches directly to MODE_PLAYER', 'ApplyLaunchArguments(lpCmdLine)' in main)
check('COM is initialized for Media Foundation/COM factories', 'CoInitializeEx(' in main and 'CoUninitialize()' in main)
check('D3D11 device enables Media Foundation video support', 'D3D11_CREATE_DEVICE_VIDEO_SUPPORT' in main)
check('24:00 synthetic duration is removed', '1440.0f' not in video)
check('native player exposes an error/status API', 'GetCPlayerStatusText' in hpp and 'CPlayerHasError' in hpp)
check('Media Foundation errors are captured', 'g_lastMediaError' in cplayer and 'MF_MEDIA_ENGINE_EVENT_ERROR' in cplayer)
check('unsafe len-1 UTF conversion is removed', "wurl.assign(len - 1" not in cplayer)
check('invalid duration no longer advances fake progress', 'io.DeltaTime / engineDuration' not in video)

set_source = cplayer.find('g_mediaEngine->SetSource(')
play = cplayer.find('g_mediaEngine->Play()', set_source)
check('native source assigns SetSource before Play',
      set_source >= 0 and play > set_source)

failed = [name for name, ok in checks if not ok]
for name, ok in checks:
    print(('PASS' if ok else 'FAIL') + ': ' + name)

if failed:
    raise SystemExit(f'{len(failed)} regression checks failed')
