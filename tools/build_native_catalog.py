"""Generate module metadata for the in-process renderer, without game access.

Settings come from the local research data (live-capture*/setting-schemas.json: display name, type, default,
bounds, step and options). Descriptions come from ui/catalog.js, matched by id. A module whose settings were not
found keeps the catalog's "unresolved" entries. The generated native/generated_catalog.hpp is committed, so this
script is only needed when that data is present.
"""
from pathlib import Path
import json, re
ROOT=Path(__file__).resolve().parents[1]
script=(ROOT/'ui/catalog.js').read_text(encoding='utf-8')
catalog=json.loads(script.split('=',1)[1].strip().rstrip(';'))
schema_path=ROOT/'live-capture/setting-schemas.json'
schemas=json.loads(schema_path.read_text(encoding='utf-8'))['modules'] if schema_path.exists() else {}
# ---- Version 2.1 data: 16 new modules; changed settings for Aim Assist, Auto Jump Reset, Disabler and MiniMap;
# Sus Chunk Finder removed (replaced by Base Finder). Values the 2.1 analysis does not read are taken from the older
# data (same setting name) or from manual-defaults.json.
V21_NEW={'auto_crystal':('Auto Crystal','Combat'),'boat_fly':('Boat Fly','Movement'),'elytra_fly':('Elytra Fly','Movement'),'long_jump':('Long Jump','Movement'),
 'pearl_chaser':('Pearl Chaser','Movement'),'reverse_step':('Reverse Step','Movement'),'strafe':('Strafe','Movement'),'base_finder':('Base Finder','Visuals'),
 'item_esp':('Item ESP','Visuals'),'item_physics':('Item Physics','Visuals'),'jump_circles':('Jump Circles','Visuals'),'sound_esp':('Sound ESP','Visuals'),
 'subtitles':('Subtitles','Visuals'),'air_place':('Air Place','Utility'),'bed_breaker':('Bed Breaker','Utility'),'custom_crosshair':('Custom Crosshair','HUD')}
V21_CHANGED={'aim_assist','auto_jump_reset','disabler','minimap'}
v21_path=ROOT/'live-capture-v21/setting-schemas.json'
if v21_path.exists():
 v21=json.loads(v21_path.read_text(encoding='utf-8'))['modules']
 manual=json.loads((ROOT/'live-capture-v21/manual-defaults.json').read_text(encoding='utf-8'))
 v21_text={}
 for line in (ROOT/'live-capture-v21/localization-en_US.txt').read_text(encoding='utf-8',errors='replace').splitlines():
  if '=' in line and not line.startswith('#'):k,v=line.split('=',1);v21_text[k]=v
 def snake_name(name):return re.sub(r'[^a-z0-9]+','_',name.lower()).strip('_')
 for mid in sorted(V21_CHANGED|set(V21_NEW)):
  if mid not in v21:continue
  olds={s['name']:s for s in schemas.get(mid,{}).get('settings',[])};merged=[]
  for s in v21[mid]['settings']:
   s=dict(s);o=olds.get(s['name'],{})
   for k in ('default','min','max','step','low','high','options','unit'):
    if s.get(k) in (None,[]) and o.get(k) not in (None,[]):s[k]=o[k]
   s.update(manual.get(mid,{}).get(s['name'],{}));merged.append(s)
  schemas[mid]=dict(schemas.get(mid,{}),settings=merged)
 have={m['id'] for m in catalog['modules']}
 for m in catalog['modules']:
  if m['id'] in V21_CHANGED:
   known={s['id'] for s in m['settings']}
   for s in schemas[m['id']]['settings']:
    sid=snake_name(s['name'])
    if sid not in known:m['settings'].append({'id':sid,'name':s['name'],'kind':s['kind'],'description':v21_text.get(f"{m['id']}.setting.{sid}.description",'')})
 for mid,(name,category) in V21_NEW.items():
  if mid in have:continue
  settings=[{'id':snake_name(s['name']),'name':s['name'],'kind':s['kind'],'description':v21_text.get(f"{mid}.setting.{snake_name(s['name'])}.description",'')} for s in schemas.get(mid,{}).get('settings',[])]
  catalog['modules'].append({'id':mid,'name':name,'description':v21_text.get(f'module.{mid}.description',''),'category':category,'settings':settings})
# UTF-8 C++ literal: non-ASCII characters are written byte by byte as octal escapes
# (\uXXXX would depend on the compiler's execution character set and break "°").
def q(s):
 out=[]
 for ch in (s or ''):
  if ch in '"\\':out.append('\\'+ch)
  elif 32<=ord(ch)<127:out.append(ch)
  else:out.extend('\\%03o'%b for b in ch.encode('utf-8'))
 return '"'+''.join(out)+'"'
def num(v,fallback=0):return repr(float(v)) if isinstance(v,(int,float)) and not isinstance(v,bool) else repr(float(fallback))
def snake(name):return re.sub(r'[^a-z0-9]+','_',name.lower()).strip('_')
# Options of lists with three or more entries are identifiers ("LockOn"); they are shown as "Lock On":
# a space is inserted between a lowercase and an uppercase letter.
def spaced(option):return re.sub(r'(?<=[a-z0-9])(?=[A-Z])',' ',option)
def row(id_,name,description,kind,low=0,high=1,step=.01,number=0,text='',options=(),unit='',rule=None):
 # visibility rule: index of the setting it depends on, direction (equal / not equal) and value; -1 = always shown
 depends=rule.get('setting') if rule and rule.get('setting') is not None else -1
 return '{'+','.join([q(id_),q(name),q(description),q(kind),num(low),num(high,1),num(step,.01),num(number),q(text),'{'+','.join(q(v) for v in options)+'}',q(unit),str(depends),'true' if not rule or rule.get('equal',True) else 'false',str(rule.get('value',0) if rule else 0)])+'}'
rows=[];recovered=0;kept=0
# Lists pre-filled by default: Xray with 20 ores, Block ESP…
defaults_path=ROOT/'live-capture/list-defaults.json'
list_defaults=json.loads(defaults_path.read_text(encoding='utf-8')) if defaults_path.exists() else {}
def list_default(module_id,index):
 lists=list_defaults.get(module_id,[]);values=lists[index] if index<len(lists) else []
 return ','.join(values) if values and all(values) else ''
# Modules that only exist as localized text and are never created are removed from the menu.
order_path=ROOT/'live-capture/module-order.json'
absent=set(json.loads(order_path.read_text(encoding='utf-8'))['catalog_modules_not_registered']) if order_path.exists() else set()
absent.add('sus_chunk_finder')   # removed in 2.1, replaced by Base Finder
# Card order: rank of the name in a fixed table; stable sort, names missing from the table go last.
card_path=ROOT/'live-capture/card-order.json'
norm=lambda n:n.lower().replace(' ','')
card={norm(n):i for i,n in enumerate(json.loads(card_path.read_text(encoding='utf-8'))['names'])} if card_path.exists() else {}
ordered=sorted(catalog['modules'],key=lambda m:card.get(norm(m['name']),len(card)))
unranked=[m['name'] for m in ordered if m['id'] not in absent and norm(m['name']) not in card]
if card:print('Card order: modules without a rank in the original table:',unranked or 'none')
for m in ordered:
 if m['id'] in absent:continue
 # category read from the data when the catalog has none (Auto Anchor, Auto Sign, Item Counter)
 if m.get('category') in (None,'Other') and schemas.get(m['id'],{}).get('category'):m=dict(m,category=schemas[m['id']]['category'])
 settings=[];known={s['id']:s for s in m['settings']};found=schemas.get(m['id'],{}).get('settings',[])
 if found:
  used={}
  for s in found:
   base=snake(s['name'] or 'setting');used[base]=used.get(base,0)+1;id_=base if used[base]==1 else f"{base}_{used[base]}"
   description=known.get(base,{}).get('description','');kind=s['kind'];recovered+=1;rule=s.get('visible_when')
   if kind in('float','int','percent'):settings.append(row(id_,s['name'],description,'number',s.get('min'),s.get('max'),s.get('step'),s.get('default'),unit=s.get('unit') or ('%' if kind=='percent' else ''),rule=rule))
   elif kind=='bool':settings.append(row(id_,s['name'],description,'bool',0,1,1,1 if s.get('default') else 0,rule=rule))
   elif kind=='enum':
    options=[spaced(o) for o in s.get('options',[])]
    settings.append(row(id_,s['name'],description,'enum' if options else 'unresolved',text=options[0] if options else '',options=options,rule=rule))
   elif kind=='range':settings.append(row(id_,s['name'],description,'range',s.get('min'),s.get('max'),s.get('step'),s.get('low') or 0,text=repr(float(s.get('high') or 0)),unit=s.get('unit',''),rule=rule))
   elif kind=='color':settings.append(row(id_,s['name'],description,'color',text=s.get('default') or '#ffffffff',rule=rule))
   elif kind=='list':settings.append(row(id_,s['name'],description,kind,text=list_default(m['id'],sum(1 for x in found[:found.index(s)] if x['kind']=='list')),rule=rule))
   else:settings.append(row(id_,s['name'],description,kind,rule=rule))
 else:
  for s in m['settings']:
   kept+=1;default=s.get('default');number=default if type(default) in (int,float) else 0
   settings.append(row(s['id'],s['name'],s.get('description'),s['kind'],s.get('min',0),s.get('max',1),s.get('step',.01),number,default if isinstance(default,str) else '',s.get('options',[])))
 rows.append('{'+','.join([q(m['id']),q(m['name']),q(m['description']),q(m['category']),'{'+','.join(settings)+'}'])+'}')
# Nickel's own settings that the reference does not have (No Fall: Glide mode, see native/game_movement2.hpp).
NICKEL_EXTRA={'{"no_fall","No Fall","Prevents fall damage","Movement",{{"falling_only","Falling Only","Only spoof while falling","bool",0.0,1.0,1.0,0.0,"",{},"",-1,true,0}}}':
 '{"no_fall","No Fall","Prevents fall damage","Movement",{{"mode","Mode","Glide: slow glide before landing; Packet: position spoof","enum",0.0,1.0,0.01,0.0,"Glide",{"Glide","Packet"},"",-1,true,0},{"falling_only","Falling Only","Only spoof while falling","bool",0.0,1.0,1.0,0.0,"",{},"",0,true,1}}}'}
rows=[NICKEL_EXTRA.get(r,r) for r in rows]
# Death Pos: Nickel's own module (not in the reference), listed after Waypoints whose entries it fills.
DEATH_POS='{"death_pos","Death Pos","Saves a waypoint where you die","Visuals",{{"keep_last","Keep Last","How many death waypoints to keep; older ones are removed","number",1.0,20.0,1.0,3.0,"",{},"",-1,true,0},{"notify","Notify","Show a notification with the saved position","bool",0.0,1.0,1.0,1.0,"",{},"",-1,true,0}}}'
rows=[x for r in rows for x in ((r,DEATH_POS) if r.startswith('{"waypoints",') else (r,))]
# Criticals: Nickel's Jump mode (real jump, works on servers that check criticals themselves).
rows=[r.replace('{"mode","Mode","How critical hits are triggered","enum",0.0,1.0,0.01,0.0,"Packet",{"Packet","Levitate","Sprint"}','{"mode","Mode","How critical hits are triggered","enum",0.0,1.0,0.01,0.0,"Packet",{"Packet","Levitate","Sprint","Jump"}') for r in rows]
# Nickel palette: accent colours (violet / blue in the reference) default to Nickel orange, tinted backgrounds to grey.
NICKEL_COLORS={'#8c59ffff': '#ff8a3dff', '#8c59ff4c': '#ff8a3d4c', '#8b41f0ff': '#ff8a3dff', '#b88cff59': '#ff8a3d59', '#d46bffff': '#ff8a3dff', '#4cccffff': '#ff8a3dff', '#59bfffff': '#ff8a3dff', '#59d9ffff': '#ff8a3dff', '#59bfffcc': '#ff8a3dcc', '#4cbfffff': '#ff8a3dff', '#4cbfff4c': '#ff8a3d4c', '#4c99ffb2': '#9494a0b2', '#14111baf': '#0f0f14af'}
for a,b in NICKEL_COLORS.items():rows=[r.replace('"'+a+'"','"'+b+'"') for r in rows]
dest=ROOT/'native/generated_catalog.hpp';dest.parent.mkdir(exist_ok=True)
dest.write_text('#pragma once\n#include <vector>\n#include <string>\nnamespace overlay {\nstruct Setting { std::string id,name,description,kind; double minimum,maximum,step,number; std::string text; std::vector<std::string> options; std::string unit; int dependsOn=-1; bool dependsEqual=true; int dependsValue=0; };\nstruct Module { std::string id,name,description,category; std::vector<Setting> settings; bool enabled=false; };\ninline std::vector<Module> modules = {\n'+',\n'.join(rows)+'\n};\n}\n',encoding='utf-8')
print('Native catalogue:',len(rows),'modules;',recovered,'settings from the recovered schemas,',kept,'kept unresolved.')
