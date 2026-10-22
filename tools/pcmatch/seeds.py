# Known PC functions and variables, from SA Mod Manager's headers (SA2Functions.h, SA2Variables.h,
# and the same macros in mods' sources): address, name, calling convention, parameter types.
import glob, os, re

FUNC_RE = re.compile(r'\b(FunctionPointer|StdcallFunctionPointer|FastcallFunctionPointer|ThiscallFunctionPointer)'
                     r'\s*\(\s*(.+?)\s*,\s*(\w+)\s*,\s*\((.*?)\)\s*,\s*(0x[0-9A-Fa-f]+)\s*\)')
VOID_RE = re.compile(r'\b(VoidFunc|ObjectFunc)\s*\(\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+)\s*\)')
DATA_RE = re.compile(r'\bDataPointer\s*\(\s*(.+?)\s*,\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+)\s*\)')
ARRAY_RE = re.compile(r'\bDataArray\s*\(\s*(.+?)\s*,\s*(\w+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*([^)]+?)\s*\)')
HOOK_RE = re.compile(r'FunctionHook<\s*([^>]*?)\s*>\s*(\w+)\s*\(\s*(0x[0-9A-Fa-f]+)')
# UsercallFunc(ret, name, (args), (callargs), addr, retreg, argregs...)
USER_RE = re.compile(r'\bUsercallFunc(?:Void)?\s*\((.*?),\s*(0x[0-9A-Fa-f]+)\s*,\s*([^;]*?)\)\s*;')

CONV = {'FunctionPointer': 'cdecl', 'StdcallFunctionPointer': 'stdcall', 'FastcallFunctionPointer': 'fastcall',
        'ThiscallFunctionPointer': 'thiscall', 'VoidFunc': 'cdecl', 'ObjectFunc': 'cdecl'}

def split_args(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '(': depth += 1
        elif ch == ')': depth -= 1
        if ch == ',' and depth == 0: out.append(cur.strip()); cur = ''
        else: cur += ch
    if cur.strip(): out.append(cur.strip())
    return [a for a in out if a and a != 'void']

def arg_type(a):
    a = re.sub(r'\b(const|volatile|struct|enum)\b', '', a).strip()
    a = re.sub(r'\s*\w+\s*$', '', a) if (' ' in a and not a.endswith('*')) else a
    return a.strip() or 'int'

def is_float(t):
    t = t.replace(' ', '')
    return t in ('float', 'double', 'Float', 'Double')

def scan(paths):
    funcs, data = {}, {}
    for path in paths:
        try: text = open(path, encoding='utf-8', errors='replace').read()
        except OSError: continue
        text = re.sub(r'//[^\n]*', '', text)
        for m in FUNC_RE.finditer(text):
            kind, ret, name, args, addr = m.groups()
            a = int(addr, 16)
            funcs.setdefault(a, {'name': name, 'conv': CONV[kind], 'ret': ret.strip(), 'args': [arg_type(x) for x in split_args(args)],
                                 'src': os.path.basename(path)})
        for m in VOID_RE.finditer(text):
            kind, name, addr = m.groups()
            funcs.setdefault(int(addr, 16), {'name': name, 'conv': 'cdecl', 'ret': 'void',
                                             'args': ['task*'] if kind == 'ObjectFunc' else [], 'src': os.path.basename(path)})
        for m in HOOK_RE.finditer(text):
            types, name, addr = m.groups()
            parts = split_args(types)
            funcs.setdefault(int(addr, 16), {'name': name.rstrip('_t').rstrip('_') or name, 'conv': 'cdecl',
                                             'ret': parts[0] if parts else 'void', 'args': parts[1:], 'src': os.path.basename(path)})
        for m in DATA_RE.finditer(text):
            t, name, addr = m.groups()
            data.setdefault(int(addr, 16), {'name': name, 'type': t.strip(), 'count': 1})
        for m in ARRAY_RE.finditer(text):
            t, name, addr, count = m.groups()
            try: n = int(eval(count, {}, {}))
            except Exception: n = 1
            data.setdefault(int(addr, 16), {'name': name, 'type': t.strip(), 'count': n})
    return funcs, data

def default_paths():
    cwe = r'C:/Users/hyper/AppData/Local/Temp/claude/C--Users-hyper/e3397157-2ca6-41da-b984-8b057bbec202/scratchpad/mods/cwe/CWE'
    paths = glob.glob(cwe + '/**/*.h', recursive=True) + glob.glob(cwe + '/**/*.cpp', recursive=True)
    paths += glob.glob('config/GSNE8P/pc_headers/*.h')
    return paths

if __name__ == '__main__':
    f, d = scan(default_paths())
    print(len(f), 'functions', len(d), 'variables')
    for a in sorted(f)[:5]: print(hex(a), f[a])
