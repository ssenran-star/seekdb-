import json, random, subprocess, sys, tempfile, time
BIN = './build/minivectordb'
N, DIM, QUERIES, K = 10000, 128, 1000, 10
random.seed(42)

class Proc:
    def __init__(self, data):
        self.p = subprocess.Popen([BIN, '--data-dir', data], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                  text=True, bufsize=1)
        assert json.loads(self.p.stdout.readline()) == {'type': 'ready'}
        self.id = 0
    def send(self, line):
        self.p.stdin.write(line); self.p.stdin.flush()
    def recv(self):
        return self.p.stdout.readline()

def vstr(v):
    return '[' + ','.join(f'{x:.6f}' for x in v) + ']'

rows = [(i, f'c{i % 10}', [random.uniform(-1, 1) for _ in range(DIM)]) for i in range(N)]
queries = []
for q in range(QUERIES):
    v = [random.uniform(-1, 1) for _ in range(DIM)]
    filt = f" WHERE category='c{q % 10}'" if q % 2 == 0 else ''
    queries.append((v, filt))

# PRE-GENERATE all request JSON lines outside the timed region
create_line = json.dumps({'id':0,'op':'execute','sql':f'CREATE TABLE bench (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR({DIM}));','search_mode':'exact'}) + '\n'
insert_lines = []
iid = 0
for b in range(N // 100):
    iid += 1
    chunk = rows[b*100:(b+1)*100]
    vals = ','.join(f"({i},'{c}','{vstr(v)}')" for i, c, v in chunk)
    insert_lines.append(json.dumps({'id':iid,'op':'execute','sql':f'INSERT INTO bench VALUES {vals};','search_mode':'exact'}) + '\n')
q_lines = []
for qi, (v, filt) in enumerate(queries):
    q_lines.append(json.dumps({'id':1000+qi,'op':'execute','sql':f"SELECT id, L2_DISTANCE(embedding,'{vstr(v)}') AS distance FROM bench{filt} ORDER BY distance LIMIT {K};",'search_mode':'exact'}) + '\n')
shutdown_line = json.dumps({'id':9999,'op':'shutdown'}) + '\n'

with tempfile.TemporaryDirectory() as d:
    p = Proc(d)
    p.send(create_line); p.recv()
    t0 = time.perf_counter()
    for line in insert_lines:
        p.send(line); p.recv()
    t_write = time.perf_counter() - t0

    # warmup both shapes
    p.send(q_lines[0]); p.recv(); p.send(q_lines[1]); p.recv()
    t_unf = t_fil = 0.0
    for (v, filt), line in zip(queries, q_lines):
        t1 = time.perf_counter()
        p.send(line); p.recv()
        dt = time.perf_counter() - t1
        if filt: t_fil += dt
        else: t_unf += dt
    p.send(shutdown_line); p.recv(); p.p.wait(timeout=30)
print(f'T_write={t_write:.3f}s  T_unfiltered={t_unf:.3f}s ({t_unf/500*1e6:.0f}us/q)  T_filtered={t_fil:.3f}s ({t_fil/500*1e6:.0f}us/q)')
