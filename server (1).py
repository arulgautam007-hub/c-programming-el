"""
server.py -- Flask-SocketIO middleware with threaded engine I/O
"""
import subprocess, threading, time, json, os, sys, queue
from flask import Flask
from flask_socketio import SocketIO, emit

app = Flask(__name__)
app.config['SECRET_KEY'] = 'tetris-secret'
socketio = SocketIO(app, cors_allowed_origins="*", async_mode='threading')

_DIR = os.path.dirname(os.path.abspath(__file__))
_BIN = os.path.join(_DIR, "tetris_engine.exe" if sys.platform=="win32" else "tetris_engine")

engine_proc  = None
resp_queue   = queue.Queue()
_ticking     = False
write_lock   = threading.Lock()

# ── stdout reader thread ──────────────────────────────────────────────────────
def reader_thread():
    """Continuously reads lines from C engine and puts them in resp_queue."""
    while True:
        try:
            if engine_proc is None or engine_proc.poll() is not None:
                time.sleep(0.05)
                continue
            line = engine_proc.stdout.readline()
            if line and line.strip():
                resp_queue.put(line.strip())
        except Exception as e:
            print("Reader error:", e)
            time.sleep(0.05)

reader = threading.Thread(target=reader_thread, daemon=True)
reader.start()

# ── engine management ─────────────────────────────────────────────────────────
def kill_engine():
    global engine_proc
    if engine_proc:
        try: engine_proc.kill()
        except: pass
        try: engine_proc.wait(timeout=1)
        except: pass
        engine_proc = None
    # drain queue
    while not resp_queue.empty():
        try: resp_queue.get_nowait()
        except: break

def launch_engine():
    global engine_proc
    kill_engine()
    if not os.path.exists(_BIN):
        print(f"ERROR: {_BIN} not found.")
        return False
    engine_proc = subprocess.Popen(
        [_BIN],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True, bufsize=1
    )
    print(f"Engine PID: {engine_proc.pid}")
    return True

def send_cmd(obj):
    """Write command to C, wait for response from reader thread."""
    if engine_proc is None or engine_proc.poll() is not None:
        print("Engine not running")
        return None
    try:
        with write_lock:
            # IMPORTANT: no spaces! json.dumps() defaults to "cmd": "start"
            # (space after the colon), but the C engine's sscanf parser
            # ("\"cmd\":\"%31[^\"]\"") requires the quote to come immediately
            # after the colon with no space. Without separators=(',', ':')
            # every command silently fails to match in C, the engine never
            # calls initGame(), and you get a valid-looking but permanently
            # empty board (score stuck at 0, no piece ever drawn).
            engine_proc.stdin.write(json.dumps(obj, separators=(',', ':')) + "\n")
            engine_proc.stdin.flush()
        # wait up to 2 seconds for response
        resp = resp_queue.get(timeout=2.0)
        return json.loads(resp)
    except queue.Empty:
        print("send_cmd: timeout waiting for engine response")
    except Exception as e:
        print("send_cmd error:", e)
    return None

# ── tick loop ─────────────────────────────────────────────────────────────────
def tick_loop():
    global _ticking
    while _ticking:
        state = send_cmd({"cmd":"tick"})
        if state:
            socketio.emit('state', state)
            if state.get('gameOver'):
                _ticking = False
                break
        time.sleep(0.1)

def start_ticking():
    global _ticking
    _ticking = True
    t = threading.Thread(target=tick_loop, daemon=True)
    t.start()

def stop_ticking():
    global _ticking
    _ticking = False
    time.sleep(0.15)

# ── socket events ─────────────────────────────────────────────────────────────
@socketio.on('connect')
def on_connect():
    print("Browser connected")

@socketio.on('start')
def on_start():
    print("START received")
    stop_ticking()
    if not launch_engine():
        return
    time.sleep(0.1)  # let reader thread attach to new process
    state = send_cmd({"cmd":"start"})
    print("First state:", bool(state))
    if state:
        emit('state', state)
        start_ticking()
    else:
        print("ERROR: no state from engine")

@socketio.on('key')
def on_key(data):
    k = data.get('key','')
    state = send_cmd({"cmd":"key","key":k})
    if state:
        emit('state', state)

# ── serve frontend ────────────────────────────────────────────────────────────
@app.route('/')
def index():
    with open(os.path.join(_DIR, 'index.html'), encoding='utf-8') as f:
        return f.read()

# ── main ─────────────────────────────────────────────────────────────────────
if __name__ == '__main__':
    if not os.path.exists(_BIN):
        print(f"Compile first: gcc tetris_engine.c -o tetris_engine.exe")
        sys.exit(1)
    print(f"Engine binary found: {_BIN}")
    print("Open http://localhost:5000")
    socketio.run(app, host='0.0.0.0', port=5000, debug=False, use_reloader=False)
