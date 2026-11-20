#!/usr/bin/env python3
"""Summarize TX optimization 0.1.0 raw data; keep failed handoff evidence visible."""
import argparse,collections,json,statistics
from pathlib import Path
def stats(values):
 values=sorted(values)
 if not values:return {'n':0}
 return {'n':len(values),'mean':statistics.mean(values),'p50':statistics.median(values),'p95':values[min(len(values)-1,int(.95*len(values)))],'p99':values[min(len(values)-1,int(.99*len(values)))],'min':values[0],'max':values[-1]}

def samples(path,raw=False):
 rows=[json.loads(s) for s in path.read_text().splitlines()];groups=collections.defaultdict(lambda:collections.defaultdict(list));previous=None
 for row in rows:
  group=groups[row['stage']];ps=row['processes'];alive=all(x['alive'] for x in ps)
  if not alive:group['dead_process_samples'].append(1)
  if alive:
   for field in ['rss','pss','hwm','threads','fds']:
    if all(x[field]>=0 for x in ps):group[field].append(sum(x[field] for x in ps))
   for index,proc in enumerate(ps):
    for field in ['rss','pss','hwm','threads','fds']:
     if proc[field]>=0:group[f'process_{index}_{field}'].append(proc[field])
   group['available_kib'].append(row['available_kib'])
   if row['cma_free_kib']>=0:group['cma_free_kib'].append(row['cma_free_kib'])
  if previous and previous['stage']==row['stage'] and alive and all(x['alive'] for x in previous['processes']):
   dt=row['t']-previous['t'];old={x['pid']:x for x in previous['processes']}
   if dt>0 and all(x['pid'] in old and x['start']==old[x['pid']]['start'] for x in ps):
    ticks=sum(x['ticks']-old[x['pid']]['ticks'] for x in ps)
    if ticks>=0:
     group['cpu_percent_one_core'].append(100*ticks/row['hz']/dt)
     for index,proc in enumerate(ps):
      delta=proc['ticks']-old[proc['pid']]['ticks']
      if delta>=0:group[f'process_{index}_cpu_percent_one_core'].append(100*delta/row['hz']/dt)
  previous=row
 if raw:return groups
 return {stage:{key:stats(v) for key,v in g.items()} for stage,g in groups.items()}
def traces(path):
    pending=collections.defaultdict(dict);active={};jobs=[]
    for line in path.read_text().splitlines():
        w=line.split()
        if len(w)!=5 or w[0]!='TXTRACE':continue
        t,stage,ticket,n=int(w[1]),w[2],int(w[3]),int(w[4])
        if stage in ('ipc_write','ipc_begin','submit'):pending[ticket][stage]=(t,n)
        elif stage=='worker_start':
            g=pending.pop(ticket,{});g[stage]=(t,n);active[ticket]=g;jobs.append(g)
        elif ticket in active:active[ticket][stage]=(t,n)
    large=[g for g in jobs if g['worker_start'][1]>=8192]
    stages={}
    for a,b in [('ipc_write','submit'),('submit','worker_start'),('ipc_begin','worker_start'),('worker_start','drain_begin'),('drain_begin','drain_end'),('drain_end','ready'),('ready','tx_done')]:
        stages[a+'->'+b]=stats([(g[b][0]-g[a][0])/1000 for g in large if a in g and b in g])
    complete=[g for g in jobs if 'ready' in g and 'tx_done' in g]
    gaps=[(b['worker_start'][0]-a['ready'][0])/1000 for a,b in zip(complete,complete[1:]) if a['worker_start'][1]>=8192 and b['worker_start'][1] in (8192,10)]
    return {'large_jobs':len(large),'stage_ms':stages,'next_worker_after_ready_ms':stats(gaps)}
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('initial',type=Path);p.add_argument('final',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();initial=json.loads((a.initial/'all.json').read_text());final=json.loads((a.final/'verify-final.json').read_text())
    out={'versions':{'old':'App 0.4.52.d / UART 1.0.26 / hd485d 0.4.0','new':'App 0.4.53.d / UART 1.0.27 / hd485d 0.4.1'},'results':{},'checks':{},'trace':{},'initial_handoff_error':initial.get('finalization_error')}
    for kind,source,folder in [('old',initial,a.initial),('new',final,a.final)]:
        peer=next(r for r in source['runs'] if r['tag']=='peer-'+kind)
        app=next(r for r in source['runs'] if r['tag']=='app-'+kind)
        groups=collections.defaultdict(list)
        for t in peer['transfers']:groups[str(t['bytes'])+'_'+t['direction']].append(t['seconds'])
        out['results'][kind]={'transfers_s':{k:stats(v) for k,v in groups.items()},'heartbeat_ms':stats(app['heartbeats_ms']),'app_resources':samples(folder/('app-'+kind+'-samples.jsonl')),'peer_resources':samples(folder/('wire-'+kind+'-samples.jsonl')),'detection':app.get('detection'),'faults':app.get('faults')}
    for path in a.initial.glob('trace-*.log'):
        if 'app' not in path.name:out['trace'][path.name]=traces(path)
    for key in ['ok','final_ok','error','abort_recovered','stall_recovered','stall','reconnect_verified_s','maintenance_ok','maintenance_subscriber','final_binary_hash','preload','final_boot','final_fds']:
        out['checks'][key]=final.get(key)
    old,new=out['results']['old'],out['results']['new']
    before=old['transfers_s']['524288_down']['p50'];after=new['transfers_s']['524288_down']['p50']
    out['difference']={'download_time_saved_percent':100*(1-after/before),'download_throughput_gain_percent':100*(before/after-1),'idle_pss_added_kib':new['app_resources']['idle']['pss']['mean']-old['app_resources']['idle']['pss']['mean'],'idle_cpu_delta_percentage_points':new['app_resources']['idle']['cpu_percent_one_core']['mean']-old['app_resources']['idle']['cpu_percent_one_core']['mean']}
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(out,indent=2)+'\n')
    print(json.dumps(out['difference'],indent=2));print('final_ok',out['checks']['final_ok'])
if __name__=='__main__':main()
