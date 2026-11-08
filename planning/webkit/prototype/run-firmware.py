from pathlib import Path
import argparse, contextlib, hashlib, json, os, shutil, subprocess, sys, time, uuid

parser=argparse.ArgumentParser(description='Run the explicitly unsandboxed M0 firmware experiment in a disposable device')
parser.add_argument('--virtualnickel',type=Path,required=True)
parser.add_argument('--memory-mib',type=int,choices=(512,768),default=768)
parser.add_argument('--cpu',choices=('max','cortex-a7'),default='max')
args=parser.parse_args()
ROOT=Path(__file__).resolve().parents[3]
VN=args.virtualnickel.resolve()
assert (VN/'tools/device.py').is_file(), 'Supply the existing local simulator checkout'
sys.path.insert(0,str(VN/'tools'))
from device import Device

output=ROOT/'build/webkit-m0'/('firmware-results-'+str(args.memory_mib)+'m-'+uuid.uuid4().hex[:10])
output.mkdir()
bundle=ROOT/'build/webkit-m0/arm-bundle'
assert (bundle/'library-inputs.tsv').is_file(), 'Build the ARM bundle first'
name='agent_kobalt_wpe_m0_'+uuid.uuid4().hex[:10]
(ROOT/'build/webkit-m0/instance-name.txt').write_text(name)
device=Device(name,engine='podman')
container='virtualnickel-'+name
engine_env=dict(os.environ,CONTAINER_ENGINE='podman')
records=[]

def run(args,**kwargs):
    return subprocess.run(args,check=True,timeout=kwargs.pop('timeout',180),**kwargs)

def cgroup():
    command=['podman','exec',container,'python3','-c',
             "from pathlib import Path; import json; print(json.dumps({p:Path('/sys/fs/cgroup',p).read_text() for p in ('memory.current','memory.peak','memory.max','memory.events','cpu.max')}))"]
    return json.loads(subprocess.check_output(command,text=True,timeout=15))

def cleanup_check(pids):
    code="""from pathlib import Path
import json,sys,time
tracked=set(json.loads(sys.argv[1])); deadline=time.monotonic()+5
while True:
    remaining=[]
    for path in Path('/proc').glob('[0-9]*/comm'):
        try:
            name=path.read_text().strip(); pid=int(path.parent.name)
            if pid in tracked or name in ('WPEWebProcess','WPENetworkProcess','WPENetworkProce','wpe-snapshot'):
                remaining.append((pid,name))
        except (OSError,ValueError): pass
    if not remaining:
        print('SUBPROCESS_CLEANUP_OK'); break
    if time.monotonic()>=deadline: raise SystemExit(str(remaining))
    time.sleep(.05)
"""
    return subprocess.check_output(['podman','exec',container,'python3','-c',code,json.dumps(pids)],text=True,timeout=12)

try:
    print('RESULTS '+str(output),flush=True)
    print('PREPARING '+name,flush=True)
    with (output/'simulator-prepare.log').open('w') as log,contextlib.redirect_stdout(log):
        device.prepare(device='libra-colour',firmware='6.0.274403')
        device.start(device='libra-colour',firmware='6.0.274403',wait=False,memory_mib=args.memory_mib)
        device.wait_library_ready(timeout=120)
    print('DEVICE_READY',flush=True)
    target=device.folder/'artifacts/kobalt-wpe-m0'
    shutil.copytree(bundle,target)
    shutil.copy2(Path(__file__).with_name('resource-sample.py'),target/'resource-sample.py')
    run(['podman','update','--cpus','1',container],stdout=subprocess.DEVNULL)
    (output/'baseline-cgroup.json').write_text(json.dumps(cgroup(),indent=2))
    device.capture('wpe-baseline')
    prefix='/instance/artifacts/kobalt-wpe-m0'
    cases=[('cold-1',False,False,2000,0,1),('cold-2',False,False,2000,0,1),
           ('metrics',True,False,10000,1,5),('crash',False,True,10000,0,1),('after-crash',False,False,2000,0,1)]
    for label,update,crash,idle,delay,sample in cases:
        temp='/tmp/kobalt-wpe-m0-'+label
        run(['podman','exec',container,'mkdir','-m','700','-p',temp])
        guest=['QEMU_LD_PREFIX=/firmware/rootfs','QEMU_CPU='+args.cpu,
               'LD_LIBRARY_PATH='+prefix+'/private:/firmware/rootfs/usr/lib:/firmware/rootfs/lib',
               'WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1',
               'KOBALT_M0_IDLE_MILLISECONDS='+str(idle),'KOBALT_M0_UPDATE_ONCE='+str(int(update)),
               'XDG_RUNTIME_DIR='+temp,'XDG_CACHE_HOME='+temp+'/cache',
               'XDG_CONFIG_HOME='+temp+'/config','XDG_DATA_HOME='+temp+'/data',
               'GIO_USE_VFS=local','__EGL_VENDOR_LIBRARY_FILENAMES=/nonexistent',
               'LIBGL_DRIVERS_PATH=/nonexistent','DISPLAY=','WAYLAND_DISPLAY=']
        command=['podman','exec','-e','KOBALT_M0_SAMPLE_SECONDS='+str(sample),
                 '-e','KOBALT_M0_SAMPLE_DELAY_SECONDS='+str(delay),
                 '-e','KOBALT_M0_KILL_WEB_PROCESS='+str(int(crash)),container,
                 'python3',prefix+'/resource-sample.py','/usr/bin/vn-offline',
                 'timeout','--kill-after=5s','90s','qemu-arm','-cpu',args.cpu,'-L','/firmware/rootfs','-U','LD_PRELOAD']
        for value in guest:
            command+=['-E',value]
        command += [prefix+'/bin/wpe-snapshot',prefix+'/fixture.xhtml',prefix+'/'+label+'.png',
                    '800','1100','--allow-unsandboxed-experiment']
        start=time.monotonic()
        result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=110)
        (output/(label+'.log')).write_text(result.stdout)
        record={'case':label,'qemu_cpu':args.cpu,'returncode':result.returncode,'wall_s':time.monotonic()-start,'cgroup':cgroup()}
        samples=[json.loads(line[len('RESOURCE_SAMPLE '):]) for line in result.stdout.splitlines() if line.startswith('RESOURCE_SAMPLE ')]
        if samples: record['resources']=samples[-1]
        pids=[row['pid'] for row in samples[-1]['processes']] if samples else []
        record['cleanup']=cleanup_check(pids).strip()
        record['temporary_disk_kib']=int(subprocess.check_output(['podman','exec',container,'du','-sk',temp],text=True).split()[0])
        record['ping']=device.command('ping')['status']
        for suffix in ('.png','.png.second.png'):
            image=target/(label+suffix)
            if image.exists():
                shutil.copy2(image,output/image.name)
                record['sha256'+suffix]=hashlib.sha256(image.read_bytes()).hexdigest()
        records.append(record)
        (output/'runs.json').write_text(json.dumps(records,indent=2))
        print('CASE '+json.dumps(record),flush=True)
        expected=1 if crash else 0
        assert result.returncode==expected,result.stdout
        assert 'INJECTED_WEB_PROCESS_CRASH' in result.stdout if crash else 'SNAPSHOT_OK' in result.stdout
        assert record['ping']=='dispatched',record
    assert records[0]['sha256.png']==records[1]['sha256.png'], 'Repeated ARM pixels differ'
    assert records[-1]['sha256.png']==records[0]['sha256.png'], 'Post-crash startup changed pixels'
    device.capture('wpe-after')
    print('FIRMWARE_PROOF_OK',flush=True)
finally:
    try:
        device.close()
        artifacts=device.folder/'artifacts'
        if artifacts.exists():
            for item in artifacts.iterdir():
                if item.is_file() and (item.suffix in ('.log','.bmp','.json') or item.name.startswith('wpe-')):
                    shutil.copy2(item,output/item.name)
    finally:
        run([str(VN/'launch.sh'),'--delete','--name',name],env=engine_env,stdout=subprocess.DEVNULL,timeout=120)
        assert not device.folder.exists(), 'Disposable instance directory remains'
        volumes=subprocess.check_output(['podman','volume','ls','-q','--filter','name=virtualnickel-system-'+name],text=True)
        assert not volumes.strip(),'Disposable volume remains'
        print('DEVICE_DELETED '+name,flush=True)
