# kgrep.py FILE REGEX : print section + offset + compact line for matches
import sys,re
f,pat=sys.argv[1],re.compile(sys.argv[2])
sec=''
for line in open(f,encoding='utf-8',errors='replace'):
    if line.startswith('==== '): sec=line[5:].split('  (')[0]; continue
    if pat.search(line):
        l=re.sub(r'object<\w+ /Script/Engine\.Default__\w+>->|\bKismet\w+Library:|\blocal |CallFunc_|_ReturnValue|self\.','',line.strip())
        print(f'[{sec[:28]}] {l[:230]}')
