# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""A small JSON Schema checker for docs/schema/x2-asset-pack-v2.schema.json, standard library only.

It implements the keywords that schema uses: $ref (local, #/...), type, enum, const, minimum, maximum, exclusiveMinimum,
exclusiveMaximum, minLength, maxLength, pattern (search, as in JSON Schema), minItems, maxItems, uniqueItems, items,
required, properties, additionalProperties, propertyNames, allOf, anyOf, oneOf, not, if/then/else, and boolean schemas.
Other keywords are ignored. The tests compare it with the `jsonschema` package when that is installed.

    validate(instance, schema) -> list of "path: message" strings (empty: valid)
"""
import json
import re


def _is_int(v):
    return (isinstance(v, int) and not isinstance(v, bool)) or (isinstance(v, float) and v.is_integer())


def _is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


_TYPES = {
    "object": lambda v: isinstance(v, dict),
    "array": lambda v: isinstance(v, list),
    "string": lambda v: isinstance(v, str),
    "integer": _is_int,
    "number": _is_num,
    "boolean": lambda v: isinstance(v, bool),
    "null": lambda v: v is None,
}


def _canon(v):
    return json.dumps(v, sort_keys=True)


def _same(a, b):
    if isinstance(a, bool) != isinstance(b, bool):
        return False
    return _canon(a) == _canon(b) if not (_is_num(a) and _is_num(b)) else a == b


def _resolve(root, ref):
    if not ref.startswith("#/"):
        raise ValueError("only local references are supported: %s" % ref)
    node = root
    for part in ref[2:].split("/"):
        node = node[part.replace("~1", "/").replace("~0", "~")]
    return node


def _check(inst, sch, path, root):
    if sch is True:
        return []
    if sch is False:
        return ["%s: not allowed here" % path]
    errs = []
    if "$ref" in sch:
        errs += _check(inst, _resolve(root, sch["$ref"]), path, root)
    if "type" in sch:
        types = sch["type"] if isinstance(sch["type"], list) else [sch["type"]]
        if not any(_TYPES[t](inst) for t in types):
            return errs + ["%s: expected %s" % (path, " or ".join(types))]
    if "const" in sch and not _same(inst, sch["const"]):
        errs.append("%s: must be %s" % (path, _canon(sch["const"])))
    if "enum" in sch and not any(_same(inst, e) for e in sch["enum"]):
        errs.append("%s: must be one of %s" % (path, ", ".join(_canon(e) for e in sch["enum"])))
    if _is_num(inst):
        if "minimum" in sch and inst < sch["minimum"]:
            errs.append("%s: below the minimum %s" % (path, sch["minimum"]))
        if "maximum" in sch and inst > sch["maximum"]:
            errs.append("%s: above the maximum %s" % (path, sch["maximum"]))
        if "exclusiveMinimum" in sch and inst <= sch["exclusiveMinimum"]:
            errs.append("%s: must be above %s" % (path, sch["exclusiveMinimum"]))
        if "exclusiveMaximum" in sch and inst >= sch["exclusiveMaximum"]:
            errs.append("%s: must be below %s" % (path, sch["exclusiveMaximum"]))
    if isinstance(inst, str):
        if "minLength" in sch and len(inst) < sch["minLength"]:
            errs.append("%s: shorter than %d" % (path, sch["minLength"]))
        if "maxLength" in sch and len(inst) > sch["maxLength"]:
            errs.append("%s: longer than %d" % (path, sch["maxLength"]))
        if "pattern" in sch and not re.search(sch["pattern"], inst):
            errs.append("%s: %r does not match %s" % (path, inst if len(inst) < 60 else inst[:57] + "...", sch["pattern"]))
    if isinstance(inst, list):
        if "minItems" in sch and len(inst) < sch["minItems"]:
            errs.append("%s: fewer than %d items" % (path, sch["minItems"]))
        if "maxItems" in sch and len(inst) > sch["maxItems"]:
            errs.append("%s: more than %d items" % (path, sch["maxItems"]))
        if sch.get("uniqueItems"):
            seen = set()
            for i in inst:
                c = _canon(i)
                if c in seen:
                    errs.append("%s: items are not unique" % path)
                    break
                seen.add(c)
        if "items" in sch:
            for i, v in enumerate(inst):
                errs += _check(v, sch["items"], "%s[%d]" % (path, i), root)
    if isinstance(inst, dict):
        for r in sch.get("required", []):
            if r not in inst:
                errs.append("%s: missing %r" % (path, r))
        props = sch.get("properties", {})
        for k, v in inst.items():
            sub = "%s.%s" % (path, k)
            if k in props:
                errs += _check(v, props[k], sub, root)
            elif "additionalProperties" in sch:
                errs += _check(v, sch["additionalProperties"], sub, root)
            if "propertyNames" in sch:
                errs += [e.replace(path + ": ", "%s (name %r): " % (path, k), 1)
                         for e in _check(k, sch["propertyNames"], path, root)]
    for sub in sch.get("allOf", []):
        errs += _check(inst, sub, path, root)
    if "anyOf" in sch and not any(not _check(inst, s, path, root) for s in sch["anyOf"]):
        errs.append("%s: matches none of the allowed forms" % path)
    if "oneOf" in sch:
        n = sum(1 for s in sch["oneOf"] if not _check(inst, s, path, root))
        if n != 1:
            errs.append("%s: must match exactly one allowed form (matches %d)" % (path, n))
    if "not" in sch and not _check(inst, sch["not"], path, root):
        errs.append("%s: matches a form that is not allowed" % path)
    if "if" in sch:
        if not _check(inst, sch["if"], path, root):
            if "then" in sch:
                errs += _check(inst, sch["then"], path, root)
        elif "else" in sch:
            errs += _check(inst, sch["else"], path, root)
    return errs


def validate(instance, schema):
    """Errors of `instance` against `schema` (a loaded JSON Schema); an empty list means valid."""
    return _check(instance, schema, "$", schema)
