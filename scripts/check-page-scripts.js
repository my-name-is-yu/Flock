// Checks the page scripts in web/assets: every name a script uses is declared or imported, no script assigns
// to an imported name, every import names something its source exports and is used, and no scripts import each other in a cycle.
// Run with: node --expose-internals scripts/check-page-scripts.js web/assets
const acorn = require('internal/deps/acorn/acorn/dist/acorn');

function patternNames(node, out = []) {
  if (!node) return out;
  switch (node.type) {
    case 'Identifier': out.push(node.name); break;
    case 'ObjectPattern': for (const p of node.properties) patternNames(p.type === 'RestElement' ? p.argument : p.value, out); break;
    case 'ArrayPattern': for (const e of node.elements) patternNames(e, out); break;
    case 'RestElement': patternNames(node.argument, out); break;
    case 'AssignmentPattern': patternNames(node.left, out); break;
  }
  return out;
}
function declared(statements) {
  const names = [];
  for (const s of statements) {
    const d = s.type === 'ExportNamedDeclaration' && s.declaration ? s.declaration : s;
    if (d.type === 'VariableDeclaration') for (const v of d.declarations) patternNames(v.id, names);
    else if ((d.type === 'FunctionDeclaration' || d.type === 'ClassDeclaration') && d.id) names.push(d.id.name);
    else if (d.type === 'ImportDeclaration') for (const sp of d.specifiers) names.push(sp.local.name);
  }
  return names;
}
function hoistedVars(node, out = []) {   // var declarations inside nested blocks of a function body
  if (!node || typeof node.type !== 'string') return out;
  if (/Function/.test(node.type)) return out;
  if (node.type === 'VariableDeclaration' && node.kind === 'var') for (const v of node.declarations) patternNames(v.id, out);
  for (const k of Object.keys(node)) {
    const c = node[k];
    if (Array.isArray(c)) c.forEach((x) => hoistedVars(x, out)); else if (c && typeof c.type === 'string') hoistedVars(c, out);
  }
  return out;
}
function analyze(root, reads, writes) {
  const bound = (scopes, name) => scopes.some((s) => s.has(name));
  function ref(name, scopes, write) { if (!bound(scopes, name)) (write ? writes : reads).add(name); }
  function pattern(node, scopes, assign) {   // assign=true: assignment target (references); false: declaration (bindings, already in scope)
    if (!node) return;
    switch (node.type) {
      case 'Identifier': if (assign) ref(node.name, scopes, true); break;
      case 'MemberExpression': visit(node, scopes); break;
      case 'ObjectPattern': for (const p of node.properties) { if (p.type === 'RestElement') pattern(p.argument, scopes, assign); else { if (p.computed) visit(p.key, scopes); pattern(p.value, scopes, assign); } } break;
      case 'ArrayPattern': for (const e of node.elements) pattern(e, scopes, assign); break;
      case 'RestElement': pattern(node.argument, scopes, assign); break;
      case 'AssignmentPattern': pattern(node.left, scopes, assign); visit(node.right, scopes); break;
      default: visit(node, scopes);
    }
  }
  function fn(node, scopes) {
    const scope = new Set();
    if (node.id && node.type !== 'FunctionDeclaration') scope.add(node.id.name);
    for (const p of node.params) patternNames(p).forEach((n) => scope.add(n));
    const inner = [...scopes, scope];
    for (const p of node.params) pattern(p, inner, false);
    if (node.body.type === 'BlockStatement') {
      declared(node.body.body).forEach((n) => scope.add(n));
      hoistedVars(node.body).forEach((n) => scope.add(n));
      for (const s of node.body.body) visit(s, inner);
    } else visit(node.body, inner);
  }
  function visit(node, scopes) {
    if (!node || typeof node.type !== 'string') return;
    switch (node.type) {
      case 'Identifier': ref(node.name, scopes, false); return;
      case 'FunctionDeclaration': case 'FunctionExpression': case 'ArrowFunctionExpression': fn(node, scopes); return;
      case 'ClassDeclaration': case 'ClassExpression': {
        const inner = [...scopes, new Set(node.id ? [node.id.name] : [])];
        visit(node.superClass, scopes);
        for (const m of node.body.body) { if (m.computed) visit(m.key, inner); visit(m.value, inner); }
        return;
      }
      case 'BlockStatement': case 'StaticBlock': { const inner = [...scopes, new Set(declared(node.body))]; node.body.forEach((s) => visit(s, inner)); return; }
      case 'SwitchStatement': {
        visit(node.discriminant, scopes);
        const inner = [...scopes, new Set(declared(node.cases.flatMap((c) => c.consequent)))];
        for (const c of node.cases) { visit(c.test, inner); c.consequent.forEach((s) => visit(s, inner)); }
        return;
      }
      case 'ForStatement': case 'ForInStatement': case 'ForOfStatement': {
        const head = node.type === 'ForStatement' ? node.init : node.left;
        const names = head && head.type === 'VariableDeclaration' ? declared([head]) : [];
        const inner = [...scopes, new Set(names)];
        if (head && head.type === 'VariableDeclaration') visit(head, inner); else if (node.type === 'ForStatement') visit(head, inner); else pattern(head, inner, true);
        visit(node.test, inner); visit(node.update, inner); visit(node.right, inner); visit(node.body, inner);
        return;
      }
      case 'CatchClause': { const inner = [...scopes, new Set(patternNames(node.param))]; pattern(node.param, inner, false); visit(node.body, inner); return; }
      case 'VariableDeclaration': for (const d of node.declarations) { pattern(d.id, scopes, false); visit(d.init, scopes); } return;
      case 'AssignmentExpression':
        if (node.left.type === 'Identifier') { ref(node.left.name, scopes, true); if (node.operator !== '=') ref(node.left.name, scopes, false); }
        else pattern(node.left, scopes, true);
        visit(node.right, scopes); return;
      case 'UpdateExpression':
        if (node.argument.type === 'Identifier') { ref(node.argument.name, scopes, true); ref(node.argument.name, scopes, false); } else visit(node.argument, scopes);
        return;
      case 'MemberExpression': visit(node.object, scopes); if (node.computed) visit(node.property, scopes); return;
      case 'Property': if (node.computed) visit(node.key, scopes); visit(node.value, scopes); return;
      case 'MethodDefinition': case 'PropertyDefinition': if (node.computed) visit(node.key, scopes); visit(node.value, scopes); return;
      case 'LabeledStatement': visit(node.body, scopes); return;
      case 'BreakStatement': case 'ContinueStatement': return;
      case 'ImportDeclaration': return;
      case 'ExportNamedDeclaration': if (node.declaration) visit(node.declaration, scopes); return;
      case 'ExportDefaultDeclaration': visit(node.declaration, scopes); return;
      case 'MetaProperty': return;
    }
    for (const k of Object.keys(node)) {
      if (k === 'type' || k === 'loc' || k === 'start' || k === 'end') continue;
      const c = node[k];
      if (Array.isArray(c)) c.forEach((x) => visit(x, scopes)); else if (c && typeof c.type === 'string') visit(c, scopes);
    }
  }
  visit(root, [new Set()]);
}
function statements(src) {
  const ast = acorn.parse(src, { ecmaVersion: 'latest', sourceType: 'module', locations: true });
  return ast.body.map((node) => {
    const reads = new Set(), writes = new Set();
    analyze(node, reads, writes);
    const names = declared([node]);
    names.forEach((n) => reads.delete(n));
    return { node, line: node.loc.start.line, names, reads, writes };
  });
}

const fs = require('fs');
const path = require('path');
const browser = new Set(`AbortController AbortSignal Array Blob Boolean CSS ClipboardItem CustomEvent Date DOMParser Element Error Event
  EventSource File FileReader FormData Headers HTMLElement Image IntersectionObserver Intl JSON Map Math MutationObserver NaN Node
  Notification Number Object Option Promise Proxy RangeError Reflect RegExp ResizeObserver Set String Symbol TextDecoder TextEncoder
  TypeError URL URLSearchParams Uint8Array WeakMap WeakSet atob btoa cancelAnimationFrame clearInterval clearTimeout console
  createImageBitmap crypto decodeURIComponent document encodeURIComponent fetch getComputedStyle globalThis history isNaN
  localStorage location matchMedia navigator parseFloat parseInt performance queueMicrotask requestAnimationFrame sessionStorage
  setInterval setTimeout structuredClone undefined window addEventListener innerHeight innerWidth`.split(/\s+/));

const directory = process.argv[2];
const scripts = fs.readdirSync(directory).filter((name) => /\.m?js$/.test(name)).sort();
const parsed = new Map(scripts.map((name) => [name, statements(fs.readFileSync(path.join(directory, name), 'utf8'))]));
const exported = new Map([...parsed].map(([name, body]) =>
  [name, new Set(body.filter((s) => s.node.type === 'ExportNamedDeclaration').flatMap((s) => s.names))]));
const problems = [];
for (const [name, body] of parsed) {
  const top = new Set(body.flatMap((s) => s.names));
  const imports = body.filter((s) => s.node.type === 'ImportDeclaration');
  const imported = new Set(imports.flatMap((s) => s.names));
  const used = new Set(body.filter((s) => s.node.type !== 'ImportDeclaration').flatMap((s) => [...s.reads, ...s.writes]));
  for (const s of body) {
    for (const n of new Set([...s.reads, ...s.writes])) if (!top.has(n) && !browser.has(n)) problems.push(`${name}:${s.line}: ${n} is not declared or imported`);
    for (const n of s.writes) if (imported.has(n)) problems.push(`${name}:${s.line}: ${n} is imported and cannot be assigned`);
  }
  for (const s of imports) {
    const source = s.node.source.value.startsWith('./') ? s.node.source.value.slice(2) : null;
    for (const specifier of s.node.specifiers) {
      if (!used.has(specifier.local.name)) problems.push(`${name}:${s.line}: ${specifier.local.name} is imported but not used`);
      if (exported.has(source) && specifier.type === 'ImportSpecifier' && !exported.get(source).has(specifier.imported.name))
        problems.push(`${name}:${s.line}: ${source} does not export ${specifier.imported.name}`);
    }
  }
}
const importsOf = new Map([...parsed].map(([name, body]) => [name, body.filter((s) => s.node.type === 'ImportDeclaration')
  .map((s) => s.node.source.value).filter((source) => source.startsWith('./')).map((source) => source.slice(2)).filter((source) => parsed.has(source))]));
const reaches = (from) => {
  const seen = new Set(), pending = [from];
  while (pending.length) for (const next of importsOf.get(pending.pop())) if (!seen.has(next)) { seen.add(next); pending.push(next); }
  return seen;
};
for (const name of scripts) if (reaches(name).has(name)) problems.push(`${name}: imports itself through ${[...reaches(name)].filter((other) => reaches(other).has(name)).sort().join(', ')}`);
if (problems.length) {
  console.error('Page script check failed');
  for (const problem of problems) console.error('  ' + problem);
  process.exit(1);
}
