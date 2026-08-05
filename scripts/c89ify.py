import re, sys

NON_DECL_KW = set('''if else for while switch case default do return break continue goto sizeof typedef enum union extern inline register volatile auto __extension__'''.split())
MODIFIERS = set('''const unsigned signed static volatile register extern inline'''.split())

# Find a trailing "NAME = ..." or "NAME;" or "NAME,..." at the end
NAME_SEP_RE = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)\s*(\[[^\]]*\])?\s*(=|;)\s*([^;]*);?$')
# Expression-like chars that cannot appear in a type-only prefix
EXPR_CHARS = re.compile(r'[\(\)\+\-\.&|/%<>=]')

def strip_strings_comments(s):
    s = re.sub(r'"(\\.|[^"\\])*"', '""', s)
    s = re.sub(r"'(\\.|[^'\\])*'", "''", s)
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    s = re.sub(r'//.*$', '', s)
    return s

def decl_match(line):
    """Match a local declaration line. Returns (name, sep, rest) or None."""
    if not re.match(r'^\s{4,}', line): return None
    if line.lstrip().startswith('#'): return None
    stripped = line.strip()
    first_tok = re.match(r'([A-Za-z_][A-Za-z0-9_]*)', stripped)
    if not first_tok: return None
    if first_tok.group(1) in NON_DECL_KW: return None
    m = NAME_SEP_RE.search(line)
    if not m: return None
    name = m.group(1)
    sep = m.group(3)
    rest = m.group(4)
    name_start = m.start(1)
    # name must be a separate token
    if name_start > 0:
        prev_ch = line[name_start - 1]
        if prev_ch not in ' \t*[,':
            return None
    type_part = line[:name_start].strip()
    if not type_part:
        return None  # e.g. `out = malloc(...)` - assignment, not declaration
    if EXPR_CHARS.search(type_part):
        return None  # e.g. `slot->index = v;` or `x + y = ...`
    # type_part must be a pure type token sequence: ident (ident|*)* with separators
    if not re.match(r'([A-Za-z_][A-Za-z0-9_]*\s+)*([A-Za-z_][A-Za-z0-9_]*\s*)?(\*+\s*)?(,\s*)?$', type_part):
        return None
    # multi-decl `type a, b;` => sep ';' but there's a top-level comma before name
    return name, sep, rest

def is_struct_member_context(lines, i):
    depth = 0
    for j in range(i, -1, -1):
        s = lines[j].strip()
        if s.startswith('};') or s == '}':
            depth += 1
        elif s.endswith('{') and (s.startswith('struct') or s.startswith('union') or s.startswith('enum')):
            if depth == 0: return True
            depth -= 1
        elif s.endswith('{') and depth == 0:
            return False
    return False

def find_block_open(lines, i):
    stack = []
    for j in range(i):
        c = strip_strings_comments(lines[j])
        for ch in c:
            if ch == '{': stack.append(j)
            elif ch == '}':
                if stack: stack.pop()
    return stack[-1] if stack else None

def is_stmt(line):
    s = line.rstrip()
    if not s.strip(): return False
    if s.strip().startswith('//') or s.strip().startswith('/*') or s.strip().startswith('*'): return False
    if s.strip().startswith('#'): return False
    if s.strip() == '{': return False
    if s.strip().startswith('{'): return False  # block start (may carry a comment)
    if s.strip() == '}': return True  # end of a block is a statement boundary
    if s.strip().endswith('{'): return False
    if decl_match(line): return False
    return True

def get_stmt_extent(lines, i):
    end_i = i
    while end_i < len(lines) and not lines[end_i].rstrip().endswith(';'):
        end_i += 1
    return end_i

def has_top_level_comma(expr):
    depth = 0
    i = 0
    n = len(expr)
    while i < n:
        ch = expr[i]
        if ch in ('"', "'"):
            q = ch
            i += 1
            while i < n and expr[i] != q:
                if expr[i] == '\\': i += 1
                i += 1
        elif ch in '([{': depth += 1
        elif ch in ')]}': depth -= 1
        elif ch == ',' and depth == 0: return True
        i += 1
    return False

def transform_file(text):
    lines = text.split('\n')
    dels = []   # (li, ei)
    repls = []  # (li, ei, new_lines)
    ins = []    # (insert_at, decl_line)
    for i in range(1, len(lines)):
        dm = decl_match(lines[i])
        if not dm: continue
        name, sep, rest = dm
        if is_struct_member_context(lines, i): continue
        prev = lines[i-1]
        if not is_stmt(prev): continue
        if prev.rstrip().endswith(',') or prev.rstrip().endswith('\\'): continue
        ob = find_block_open(lines, i)
        if ob is None: continue
        insert_at = ob + 1
        end_i = get_stmt_extent(lines, i)
        if sep == ';' and ',' in lines[i][:lines[i].find(name)]:
            # multi-decl `type a, b;` - move the whole line to the block top
            block_indent = len(lines[ob]) - len(lines[ob].lstrip())
            ins.append((insert_at, [' ' * (block_indent + 4) + lines[i].lstrip()]))
            dels.append((i, end_i))
            continue
        if sep == '=' and has_top_level_comma(rest):
            continue
        # duplicate check
        already = False
        for j in range(insert_at, i):
            dj = decl_match(lines[j])
            if dj and dj[0] == name:
                already = True
                break
        if already: continue
        nm2 = NAME_SEP_RE.search(lines[i])
        type_part = lines[i][:nm2.start(1)].strip()
        full_decl = lines[i:end_i+1]
        full_text = '\n'.join(full_decl)
        if 'static' in type_part or rest.lstrip().startswith('{'):
            # Static or aggregate initializers cannot be split into
            # declaration + assignment: move the whole declaration to the
            # block top (semantics preserved; C89 requires it at block top).
            block_indent = len(lines[ob]) - len(lines[ob].lstrip())
            moved = [' ' * (block_indent + 4) + lines[i].lstrip()] + lines[i+1:end_i+1]
            ins.append((insert_at, moved))
            dels.append((i, end_i))
            continue
        ob_line = lines[ob]
        base = len(ob_line) - len(ob_line.lstrip()) + 4
        # try to align with the first following line's indent
        if i + 1 < len(lines) and lines[i+1].strip():
            nxt = len(lines[i+1]) - len(lines[i+1].lstrip())
            if 4 <= nxt <= 40:
                base = nxt
        arr = NAME_SEP_RE.search(lines[i]).group(2) or ''
        decl_line = ' ' * base + type_part + ' ' + name + arr + ';'
        ins.append((insert_at, [decl_line]))
        if sep == '=':
            nm = NAME_SEP_RE.search(lines[i])
            indent = lines[i][:len(lines[i]) - len(lines[i].lstrip())]
            after_name = lines[i][nm.end(1):]  # '[16] = ...' or ' = ...'
            eq_pos = after_name.find('=')
            assign = indent + name + after_name[:eq_pos] + '=' + after_name[eq_pos+1:]
            cont = [assign] + lines[i+1:end_i+1]
            repls.append((i, end_i, cont))
        else:
            dels.append((i, end_i))
    if not (dels or repls or ins): return text, 0
    # Single-pass rebuild using original indices
    del_by_start = {d[0]: d for d in dels}
    repl_by_start = {r[0]: r for r in repls}
    ins_by_pos = {}
    for pos, ilines in ins:
        ins_by_pos.setdefault(pos, []).extend(ilines)
    out = []
    i = 0
    n = len(lines)
    while i < n:
        if i in ins_by_pos:
            out.extend(ins_by_pos[i])
        if i in del_by_start:
            i = del_by_start[i][1] + 1
            continue
        if i in repl_by_start:
            li, ei, newl = repl_by_start[i]
            out.extend(newl)
            i = ei + 1
            continue
        out.append(lines[i])
        i += 1
    return '\n'.join(out), len(ins)

for fn in sys.argv[1:]:
    with open(fn) as f: orig = f.read()
    cur = orig
    total = 0
    for round_ in range(50):
        new, n = transform_file(cur)
        total += n
        if n == 0:
            break
        cur = new
    if total:
        with open(fn,'w') as f: f.write(cur)
        print(f"{fn}: hoisted {total} (rounds: {round_+1})")
