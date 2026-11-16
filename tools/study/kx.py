# kx.py FILE START_OFFSET MAXLINES  -> compact listing of ubergraph from an offset
import sys,re
f,start,n=sys.argv[1],int(sys.argv[2]),int(sys.argv[3])
sec=sys.argv[4] if len(sys.argv)>4 else 'ExecuteUbergraph'
subs=[(r'object<\w+ /Script/Engine\.Default__\w+>->',''),(r'\bKismet(Math|System|Array|String)Library:',''),(r'\bBlueprintMapLibrary:',''),
(r'\blocal ',''),(r'CallFunc_',''),(r'_ReturnValue',''),(r'K2Node_',''),(r'_ImplicitCast',''),(r'self\.',''),(r'\bTemp_\w+?_Variable','T')]
on=False;out=0
for line in open(f,encoding='utf-8',errors='replace'):
    if line.startswith('==== '): on = line.startswith('==== '+sec); continue
    if not on: continue
    m=re.match(r'\s*(\d+):',line)
    if m and int(m.group(1))<start and out==0: continue
    for a,b in subs: line=re.sub(a,b,line)
    sys.stdout.write(line[:300].rstrip()+'\n'); out+=1
    if out>=n: break
