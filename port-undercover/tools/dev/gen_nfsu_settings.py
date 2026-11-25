# Writes mods/core/nfsu/mod.toml's [settings] from the table below (English + Russian, detailed hints).
import json, sys
S = []
def add(key, typ, default, apply, group, label, label_ru, help, help_ru, choices=None, choices_ru=None, mn=None, mx=None,
        step=None, unit=None):
    S.append(dict(key=key, typ=typ, default=default, apply=apply, group=group, label=label, label_ru=label_ru,
                  help=help, help_ru=help_ru, choices=choices, choices_ru=choices_ru, mn=mn, mx=mx, step=step, unit=unit))
exec(open(sys.argv[1], encoding="utf-8").read())
def q(s):
    # the kit's TOML reader takes no escapes: a double quote or backslash inside a text rejects the whole mod
    assert '"' not in s and "\\" not in s, s
    return json.dumps(s, ensure_ascii=False)
lines = []
for s in S:
    parts = [f'type = {q(s["typ"])}', f'default = {str(s["default"]).lower() if s["typ"]=="bool" else s["default"]}']
    if s["typ"] == "int":
        parts += [f'min = {s["mn"]}', f'max = {s["mx"]}']
        if s["step"]:
            parts += [f'step = {s["step"]}']
        if s["unit"]:
            parts += [f'unit = {q(s["unit"])}']
        if s["choices"]:
            assert s["choices"].count("|") == s["mx"] - s["mn"] == s["choices_ru"].count("|"), s["key"]
            parts += [f'choices = {q(s["choices"])}', f'choices_ru = {q(s["choices_ru"])}']
    parts += [f'apply = {q(s["apply"])}', f'label = {q(s["label"])}', f'label_ru = {q(s["label_ru"])}',
              f'group = {q(s["group"])}', f'help = {q(s["help"])}', f'help_ru = {q(s["help_ru"])}']
    assert len(s["help"].encode()) <= 1200 and len(s["help_ru"].encode()) <= 2400, s["key"]
    lines.append(f'{s["key"]} = {{ ' + ", ".join(parts) + " }")
p = "mods/core/nfsu/mod.toml"
t = open(p, encoding="utf-8").read()
head = t[:t.index("\n[settings]\n") + 1]
open(p, "w", encoding="utf-8").write(head + "[settings]\n" + "\n".join(lines) + "\n")
print(len(S), "settings written")
