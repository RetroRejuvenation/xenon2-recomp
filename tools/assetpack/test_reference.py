#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the xenon2-recomp authors
"""Keeps docs/ASSET-PACKS-REFERENCE.md in step with docs/schema/x2-asset-pack-v2.schema.json.

    python3 -I tools/assetpack/test_reference.py

The reference has one table per kind of object. A comment line `<!-- fields: PATH [PATH...] -->` names the schema object(s) the
next table documents; the first column of each row is the field name in backticks. The test requires:

  * every object of the schema that has named properties is documented, and every documented path exists;
  * the fields of each table are exactly the properties of the object;
  * a required field says "required" in its row;
  * every value of an enum, every range limit and every length limit of a property appears in its row;
  * every complete manifest (a ```json block with "format": "x2-asset-pack") in the documents under docs/ and in the
    examples validates against the schema.

PATH is written like `graphics{}` (the values of an object used as a map), `far[]` (the items of an array) and `a.b` (a
property); the manifest itself is `root`.
"""
import json
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import minischema as MS  # noqa: E402

SCHEMA = os.path.join(ROOT, "docs", "schema", "x2-asset-pack-v2.schema.json")
REFERENCE = os.path.join(ROOT, "docs", "ASSET-PACKS-REFERENCE.md")
MARKER = re.compile(r"^<!--\s*fields:\s*(.+?)\s*-->\s*$")


def load_schema():
    with open(SCHEMA, encoding="utf-8") as f:
        return json.load(f)


def resolve(node, schema):
    seen = 0
    while isinstance(node, dict) and "$ref" in node:
        ref = node["$ref"]
        assert ref.startswith("#/"), ref
        target = schema
        for part in ref[2:].split("/"):
            target = target[part]
        node = target
        seen += 1
        assert seen < 20, "reference loop"
    return node


def object_options(node, schema):
    """The object schemas a node can be: itself, or the object members of its oneOf/anyOf."""
    node = resolve(node, schema)
    out = []
    if isinstance(node, dict):
        if "properties" in node:
            out.append(node)
        for key in ("oneOf", "anyOf"):
            for alt in node.get(key, []):
                alt = resolve(alt, schema)
                if isinstance(alt, dict) and "properties" in alt:
                    out.append(alt)
    return out


def walk(node, path, schema, out):
    """Collect {path: object schema} for every object with named properties below `node`."""
    node = resolve(node, schema)
    if not isinstance(node, dict):
        return
    for obj in object_options(node, schema):
        out.setdefault(path, obj)
        for name, sub in obj["properties"].items():
            walk(sub, name if path == "root" else path + "." + name, schema, out)
    if node.get("type") == "array" and "items" in node:
        walk(node["items"], path + "[]", schema, out)
    ap = node.get("additionalProperties")
    if isinstance(ap, dict):
        walk(ap, path + "{}", schema, out)


def schema_objects(schema):
    out = {}
    walk(schema, "root", schema, out)
    # the same object reached twice (sfx and speech are both a clip) stays documented under each path
    return out


def number_text(n):
    return str(int(n)) if float(n).is_integer() else repr(n)


def constraints(prop, schema):
    """Numbers a row has to mention: ranges and lengths of the property and of the items of an array property."""
    found = set()

    def visit(node, depth=0):
        node = resolve(node, schema)
        if not isinstance(node, dict) or depth > 3:
            return
        for key in ("minimum", "maximum", "exclusiveMinimum", "maxItems", "maxLength"):
            if key in node:
                found.add(number_text(node[key]))
        for key in ("minItems", "minLength"):
            if key in node and node[key] > 1:
                found.add(number_text(node[key]))
        for alt in node.get("oneOf", []) + node.get("anyOf", []):
            visit(alt, depth + 1)
        if "items" in node and isinstance(node["items"], dict):
            visit(node["items"], depth + 1)

    visit(prop)
    return found


def enum_values(prop, schema):
    found = []

    def visit(node, depth=0):
        node = resolve(node, schema)
        if not isinstance(node, dict) or depth > 3:
            return
        if "enum" in node:
            found.extend(v for v in node["enum"] if isinstance(v, str))
        if "const" in node and isinstance(node["const"], str):
            found.append(node["const"])
        for alt in node.get("oneOf", []) + node.get("anyOf", []):
            visit(alt, depth + 1)
        if "items" in node and isinstance(node["items"], dict):
            visit(node["items"], depth + 1)

    visit(prop)
    return found


def parse_reference(text):
    """{path: {field: row text}} for every marked table, in the document."""
    tables = {}
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        m = MARKER.match(lines[i])
        if not m:
            i += 1
            continue
        paths = m.group(1).split()
        i += 1
        while i < len(lines) and not lines[i].startswith("|"):
            i += 1
        rows = {}
        header_seen = 0
        while i < len(lines) and lines[i].startswith("|"):
            header_seen += 1
            if header_seen > 2:
                cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
                name = cells[0].strip("`")
                rows[name] = lines[i]
            i += 1
        for p in paths:
            tables[p] = rows
    return tables


def json_blocks(text):
    return re.findall(r"```json\n(.*?)\n```", text, re.S)


class ReferenceMatchesSchema(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = load_schema()
        cls.objects = schema_objects(cls.schema)
        with open(REFERENCE, encoding="utf-8") as f:
            cls.tables = parse_reference(f.read())

    def test_every_object_is_documented_and_nothing_else(self):
        self.assertEqual(sorted(self.objects), sorted(self.tables))

    def test_fields_match(self):
        for path, obj in self.objects.items():
            rows = self.tables.get(path, {})
            self.assertEqual(sorted(rows), sorted(obj["properties"]), "fields of %s" % path)

    def test_required_fields_say_so(self):
        for path, obj in self.objects.items():
            rows = self.tables.get(path, {})
            for name in obj.get("required", []):
                self.assertIn("required", rows.get(name, "").lower(), "%s.%s must say required" % (path, name))
            for name in obj["properties"]:
                if name not in obj.get("required", []):
                    self.assertNotIn("**required**", rows.get(name, ""), "%s.%s is not required" % (path, name))

    def test_enums_and_limits_are_stated(self):
        for path, obj in self.objects.items():
            rows = self.tables.get(path, {})
            for name, prop in obj["properties"].items():
                row = rows.get(name, "").replace(",", "")
                for v in enum_values(prop, self.schema):
                    self.assertTrue(re.search(r"(?<![A-Za-z0-9_])" + re.escape(v) + r"(?![A-Za-z0-9_])", row),
                                    "%s.%s: the value %r is not in its row" % (path, name, v))
                for n in constraints(prop, self.schema):
                    self.assertTrue(re.search(r"(?<![0-9.])" + re.escape(n) + r"(?![0-9])", row), "%s.%s: the limit %s is not in its row" % (path, name, n))


class ExamplesValidate(unittest.TestCase):
    def test_manifest_examples_in_the_documents_validate(self):
        schema = load_schema()
        docs = [os.path.join(ROOT, "docs", n) for n in ("ASSET-PACKS.md", "ASSET-PACKS-REFERENCE.md")]
        tut = os.path.join(ROOT, "docs", "asset-packs")
        if os.path.isdir(tut):
            docs += [os.path.join(tut, n) for n in sorted(os.listdir(tut)) if n.endswith(".md")]
        checked = 0
        for path in docs:
            if not os.path.exists(path):
                continue
            with open(path, encoding="utf-8") as f:
                text = f.read()
            for block in json_blocks(text):
                if '"format": "x2-asset-pack"' not in block and '"format":"x2-asset-pack"' not in block:
                    continue
                try:
                    manifest = json.loads(block)
                except ValueError as e:
                    self.fail("%s: an example manifest is not JSON (%s):\n%s" % (os.path.relpath(path, ROOT), e, block[:300]))
                errs = MS.validate(manifest, schema)
                self.assertEqual(errs, [], "%s: an example manifest does not validate" % os.path.relpath(path, ROOT))
                checked += 1
        self.assertGreater(checked, 3, "the documents hold very few complete example manifests")


if __name__ == "__main__":
    if "--paths" in sys.argv:
        for p in sorted(schema_objects(load_schema())):
            print(p)
        sys.exit(0)
    unittest.main()
