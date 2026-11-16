"""RealRTCW VR Hand Editor: a local page for posing the hands on each VR weapon.

Run by "Start Hand Editor.cmd". Serves the editor on 127.0.0.1 only. Never launches the game
or a VR runtime. Standard library only.
"""
import argparse, copy, hashlib, json, os, secrets, shutil, subprocess, sys, threading, time, traceback, webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, unquote

import rtcw_hands as H

WEB = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.normpath(os.path.join(WEB, '..', '..'))            # <RealRTCW>\game
MOD_MAIN = os.path.join(GAME, 'main')
DATA = os.path.join(WEB, 'data')
EDITS = os.path.join(DATA, 'hand_edits.json')
BUILT = os.path.join(DATA, H.OUT_PK3)
RECEIPT = os.path.join(DATA, 'built.json')
TOKEN = secrets.token_urlsafe(32)
LOCK = threading.RLock()
JOB = dict(application='realrtcw-hand-editor', running=False, message='Ready', log='')
PAKS = CACHE = None


def base_dirs():
    """RealRTCW's own Main (the Steam install this mod sits in), then the mod's game\\main."""
    cands = [os.path.join(GAME, '..', 'Main'), os.path.join(GAME, '..', 'main')]
    rr = os.environ.get('RRTCW')
    if rr:
        cands.insert(0, os.path.join(rr, 'Main'))
    base = next((os.path.normpath(c) for c in cands if os.path.isdir(c)), None)
    if not base:
        raise SystemExit('Could not find RealRTCW\'s Main folder next to the game folder.')
    return [base, MOD_MAIN]


def atomic_json(path, value):
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(value, f, indent=1)
    os.replace(tmp, path)


def digest_bytes(b):
    return hashlib.sha256(b).hexdigest()


def digest(path):
    with open(path, 'rb') as f:
        return digest_bytes(f.read())


def saved_edits():
    if os.path.exists(EDITS):
        return H.validate(json.load(open(EDITS, encoding='utf-8')))
    return dict(version=1, weapons={})


def edits_digest(edits):
    return digest_bytes(json.dumps(edits, sort_keys=True).encode())


def logline(text):
    with LOCK:
        JOB['log'] = (JOB['log'] + text + '\n')[-14000:]
        JOB['message'] = text
    print(text, flush=True)


def game_running():
    if os.name != 'nt':
        return False
    try:
        out = subprocess.run(['tasklist', '/FO', 'CSV', '/NH'], capture_output=True, text=True,
                             creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)).stdout.lower()
    except OSError:
        return False
    return 'realrtcw.x64.exe' in out or 'realrtcw.exe' in out


def build():
    edits = saved_edits()
    logline('Building %s from your saved poses...' % H.OUT_PK3)
    files = H.bake(PAKS, CACHE, edits, log=logline)
    os.makedirs(DATA, exist_ok=True)
    H.write_pk3(BUILT, files, edits)
    atomic_json(RECEIPT, dict(edits=edits_digest(edits), pk3=digest(BUILT), files=len(files)))
    logline('Build complete (%d model files). Close the game, then press Install.' % len(files))


def install():
    if game_running():
        raise RuntimeError('RealRTCW is running. Close the game first, then press Install again.')
    if not os.path.exists(RECEIPT) or not os.path.exists(BUILT):
        raise RuntimeError('Build saved poses first')
    receipt = json.load(open(RECEIPT))
    edits = saved_edits()
    if receipt['edits'] != edits_digest(edits) or receipt['pk3'] != digest(BUILT):
        raise RuntimeError('Your poses changed since the last build. Build again before installing.')
    backup = new_backup_dir()
    pk3 = os.path.join(MOD_MAIN, H.OUT_PK3)
    align = os.path.join(MOD_MAIN, 'vr_handalign.txt')
    for path in (pk3, align):
        if os.path.exists(path):
            os.makedirs(backup, exist_ok=True)
            shutil.copy2(path, os.path.join(backup, os.path.basename(path)))
    if os.path.isdir(backup):
        logline('Backed up the previous files to ' + backup)
    tmp = pk3 + '.editor-pending'
    shutil.copy2(BUILT, tmp)
    if digest(tmp) != digest(BUILT):
        os.remove(tmp)
        raise RuntimeError('Copy check failed; nothing was installed')
    os.replace(tmp, pk3)
    H.update_align_file(align, edits)
    logline('Installed %s and weapon sizes into game\\main. Start the game as usual. No game was started.' % H.OUT_PK3)


def uninstall():
    if game_running():
        raise RuntimeError('RealRTCW is running. Close the game first.')
    pk3 = os.path.join(MOD_MAIN, H.OUT_PK3)
    if not os.path.exists(pk3):
        logline('No hand edits are installed.')
        return
    backup = new_backup_dir()
    os.makedirs(backup, exist_ok=True)
    shutil.move(pk3, os.path.join(backup, H.OUT_PK3))
    logline('Removed the installed hand edits (moved to %s). Your saved poses are kept.' % backup)


def new_backup_dir():
    base = os.path.join(MOD_MAIN, 'backup-%s-hand-editor' % time.strftime('%Y-%m-%d-%H%M%S'))
    path, n = base, 2
    while os.path.exists(path):
        path, n = '%s-%d' % (base, n), n + 1
    return path


def start_job(action):
    with LOCK:
        if JOB['running']:
            raise ValueError('A build/install is already running')
        JOB.update(running=True, message='Starting ' + action, log='')

    def worker():
        try:
            {'build': build, 'install': install, 'uninstall': uninstall}[action]()
        except Exception as error:
            with LOCK:
                JOB.update(message=str(error), log=JOB['log'] + '\n' + traceback.format_exc())
        finally:
            with LOCK:
                JOB['running'] = False
    threading.Thread(target=worker, daemon=True).start()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, value, status=200, mime='application/json'):
        data = json.dumps(value, separators=(',', ':')).encode() if mime == 'application/json' else value
        self.send_response(status)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' blob: data:; connect-src 'self'; object-src 'none'; frame-ancestors 'none'")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        try:
            path = urlsplit(self.path).path
            if self.headers.get('Host') != '127.0.0.1:%d' % self.server.server_port:
                return self.reply({'error': 'Invalid host'}, 403)
            if path == '/api/state':
                edits = saved_edits()
                installed = os.path.exists(os.path.join(MOD_MAIN, H.OUT_PK3))
                return self.reply(dict(token=TOKEN, catalog=H.catalog(PAKS), edits=edits, revision=edits_digest(edits),
                                       installed=installed, job=copy.copy(JOB)))
            if path == '/api/status':
                return self.reply(copy.copy(JOB))
            if path.startswith('/api/weapon/'):
                weap = unquote(path.rsplit('/', 1)[1])
                if weap not in H.TABLES['poses']:
                    raise ValueError('Unknown weapon')
                return self.reply(CACHE.view(weap))
            rel = 'index.html' if path == '/' else path.lstrip('/')
            target = os.path.realpath(os.path.join(WEB, rel))
            if not target.startswith(os.path.realpath(WEB) + os.sep) or not os.path.isfile(target) or \
                    os.path.splitext(target)[1] not in ('.html', '.js', '.css', '.txt', '.json') or '/data/' in target.replace('\\', '/'):
                return self.reply({'error': 'Not found'}, 404)
            mime = {'.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
                    '.txt': 'text/plain; charset=utf-8', '.json': 'application/json'}[os.path.splitext(target)[1]]
            self.reply(open(target, 'rb').read(), mime=mime)
        except Exception as e:
            self.reply({'error': str(e)}, 400)

    def do_POST(self):
        try:
            if self.headers.get('Origin') != 'http://127.0.0.1:%d' % self.server.server_port or self.headers.get('X-Editor-Token') != TOKEN:
                return self.reply({'error': 'Editor restarted. Export any unsaved edits, then refresh this tab.'}, 403)
            n = int(self.headers.get('Content-Length', '0'))
            if not 0 < n < 2 * 1024 * 1024:
                raise ValueError('Invalid request size')
            data = json.loads(self.rfile.read(n))
            path = urlsplit(self.path).path
            if path == '/api/preview':
                weap, side = data['id'], data['side']
                w = CACHE.get(weap)
                if side not in w['rigs']:
                    raise ValueError('No such hand')
                edit = H.validate_hand(data['edit'])
                return self.reply(w['rigs'][side].preview(edit))
            if path == '/api/validate':
                return self.reply(H.validate(data))
            if path == '/api/save':
                edits = H.validate(data['edits'])
                with LOCK:
                    if JOB['running']:
                        raise ValueError('Wait for the build/install to finish')
                    current = edits_digest(saved_edits())
                    if current != data.get('revision'):
                        return self.reply({'error': 'Saved poses changed outside this tab. Export your edits, then refresh.'}, 409)
                    os.makedirs(DATA, exist_ok=True)
                    if os.path.exists(EDITS):
                        shutil.copy2(EDITS, os.path.join(DATA, 'hand_edits.previous.json'))
                    atomic_json(EDITS, edits)
                return self.reply(dict(message='Saved to tools\\hand-editor\\data\\hand_edits.json', revision=edits_digest(edits)))
            if path in ('/api/build', '/api/install', '/api/uninstall'):
                start_job(path.rsplit('/', 1)[1])
                return self.reply(dict(message='Started'))
            if path == '/api/shutdown':
                with LOCK:
                    if JOB['running']:
                        raise ValueError('Wait for the build/install to finish first')
                self.reply(dict(message='Editor stopped'))
                threading.Thread(target=self.server.shutdown, daemon=True).start()
                return
            self.reply({'error': 'Not found'}, 404)
        except Exception as e:
            self.reply({'error': str(e)}, 400)


def main():
    global PAKS, CACHE
    ap = argparse.ArgumentParser()
    ap.add_argument('--no-browser', action='store_true')
    ap.add_argument('--port', type=int, default=8766)
    args = ap.parse_args()
    os.makedirs(DATA, exist_ok=True)
    info = os.path.join(DATA, 'server.json')
    if os.path.exists(info):
        try:
            from urllib.request import urlopen
            url = json.load(open(info))['url']
            with urlopen(url + 'api/status', timeout=1) as r:
                if json.load(r).get('application') == 'realrtcw-hand-editor':
                    print('Editor already running: ' + url, flush=True)
                    if not args.no_browser:
                        webbrowser.open(url)
                    return
        except (OSError, ValueError, KeyError):
            pass
    dirs = base_dirs()
    print('Reading weapons from:\n  ' + '\n  '.join(dirs), flush=True)
    PAKS = H.Paks(dirs)
    CACHE = H.WeaponCache(PAKS)
    if not H.catalog(PAKS):
        raise SystemExit('No RealRTCW weapons found. Is this inside the RealRTCW VR mod\'s game folder?')
    try:
        server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    except OSError:
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    url = 'http://127.0.0.1:%d/' % server.server_port
    atomic_json(info, dict(url=url, pid=os.getpid()))
    print('RealRTCW VR Hand Editor: ' + url, flush=True)
    print('Close this window to stop the editor. The game is never started from here.', flush=True)
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
