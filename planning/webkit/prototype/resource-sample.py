from pathlib import Path
import ctypes, json, os, subprocess, sys, time


def snapshot(root):
    table={}
    for path in Path('/proc').glob('[0-9]*/stat'):
        try:
            text=path.read_text()
            fields=text.rsplit(')',1)[1].split()
            table[int(path.parent.name)]={'parent':int(fields[1]), 'ticks':int(fields[11])+int(fields[12]), 'name':text.split('(',1)[1].rsplit(')',1)[0]}
        except (OSError,ValueError,IndexError):
            pass
    selected={root}
    while True:
        more={pid for pid,item in table.items() if item['parent'] in selected}
        if more<=selected:
            break
        selected|=more
    rows=[]
    errors=[]
    for pid in sorted(selected):
        if pid not in table:
            continue
        row={'pid':pid, **table[pid]}
        try:
            memory={}
            for line in Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines():
                parts=line.split()
                if len(parts)>=2 and parts[0] in ('Rss:','Pss:','Private_Clean:','Private_Dirty:'):
                    memory[parts[0][:-1]]=int(parts[1])
            row.update(memory)
        except OSError as error:
            errors.append(str(error))
        try:
            names={Path(line.split()[-1]).name for line in Path(f'/proc/{pid}/maps').read_text().splitlines() if '/' in line}
            row['graphics_modules']=sorted(name for name in names if name.startswith(('libEGL','libGLES','libGL.','libGLX','libOpenGL','libvulkan','libgbm','libdrm','libMali','libgst')))
        except OSError as error:
            errors.append(str(error))
        rows.append(row)
    return rows,errors

# Native run-owned supervisor: collect orphaned QEMU/WebKit children as well.
libc=ctypes.CDLL(None,use_errno=True)
if libc.prctl(36,1,0,0,0):
    raise OSError(ctypes.get_errno(),'PR_SET_CHILD_SUBREAPER')
launch_started=time.monotonic()
process=subprocess.Popen(sys.argv[1:],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1)
for line in process.stdout:
    print(line,end='',flush=True)
    if line.strip()=='IDLE_READY':
        controller_to_idle=time.monotonic()-launch_started
        settle_until=time.monotonic()+float(os.environ.get('KOBALT_M0_SAMPLE_DELAY_SECONDS','0'))
        while time.monotonic()<settle_until:
            time.sleep(.05)
        before,errors=snapshot(process.pid)
        started=time.monotonic()
        until=started+float(os.environ.get('KOBALT_M0_SAMPLE_SECONDS','1'))
        while time.monotonic()<until:
            time.sleep(.05)
        after,after_errors=snapshot(process.pid)
        elapsed=time.monotonic()-started
        initial={row['pid']:row['ticks'] for row in before}
        ticks=sum(row['ticks']-initial.get(row['pid'],row['ticks']) for row in after)
        report={'controller_to_idle_s':controller_to_idle,'elapsed_s':elapsed,'idle_cpu_percent':100*ticks/os.sysconf('SC_CLK_TCK')/elapsed,
                'pss_kib':sum(row.get('Pss',0) for row in before) if not errors else None,
                'rss_kib':sum(row.get('Rss',0) for row in before),'processes':before,
                'errors':errors+after_errors,'process_set_changed':set(initial)!={row['pid'] for row in after}}
        print('RESOURCE_SAMPLE '+json.dumps(report),flush=True)
        if os.environ.get('KOBALT_M0_KILL_WEB_PROCESS')=='1':
            victims=[]
            for row in after:
                try:
                    args=Path(f'/proc/{row["pid"]}/cmdline').read_bytes().split(b'\0')
                    if row['name']=='WPEWebProcess' or any(Path(arg.decode(errors='replace')).name=='WPEWebProcess' for arg in args if arg):
                        victims.append(row['pid'])
                except OSError:
                    pass
            if len(victims)!=1:
                raise SystemExit('Cannot identify exactly one owned WebProcess for fault injection')
            os.kill(victims[0],9)
            print('INJECTED_WEB_PROCESS_CRASH '+str(victims[0]),flush=True)
result=process.wait()
reaped=[]
deadline=time.monotonic()+5
while True:
    try:
        pid,status=os.waitpid(-1,os.WNOHANG)
    except ChildProcessError:
        print('SUPERVISOR_REAPED '+json.dumps(reaped),flush=True)
        break
    if pid:
        reaped.append({'pid':pid,'status':status})
        continue
    if time.monotonic()>=deadline:
        remaining,errors=snapshot(os.getpid())
        print('SUPERVISOR_CLEANUP_FAILED '+json.dumps({'processes':remaining,'errors':errors}),flush=True)
        raise SystemExit(3)
    time.sleep(.05)
raise SystemExit(result)
