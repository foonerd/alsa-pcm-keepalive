#!/bin/bash
# The pass CI runs and every commit passes: the C built with warnings denied and through
# GCC's static analyzer, the tests against a clocked fake device, the shell scripts
# parsed and linted, the plugin's JSON and script parsed, every translation carrying the
# English keys in the English order, the version named once, and the workflows through
# actionlint.
set -euo pipefail
ROOT=$(CDPATH='' cd -- "$(dirname "$0")/.." && pwd)
cd "$ROOT"

echo "check: C, warnings denied, tests"
scripts/builder.sh sh -c 'make -s clean && PYTHONDONTWRITEBYTECODE=1 make -s test; rc=$?; make -s clean; exit $rc'
echo "check: C, static analyzer"
scripts/builder.sh sh -c 'for f in keepalive_common.c keepalive_daemon.c libasound_module_pcm_keepalive.c; do
  gcc -fanalyzer -Wall -Wextra -Werror -O2 -fPIC -DPIC -c -o /dev/null "$f" || exit 1
done'

echo "check: shell scripts"
for script in scripts/*.sh plugin/*.sh; do
  sh -n "$script" || { echo "check: $script does not parse" >&2; exit 1; }
done
if command -v shellcheck >/dev/null 2>&1; then
  # The scripts the player runs go through its sh: POSIX sh, no bash.
  shellcheck -S warning -s sh plugin/install.sh plugin/uninstall.sh
  shellcheck -S warning scripts/*.sh
else
  echo "check: shellcheck is not installed here; CI's shell job runs it"
fi

# Node runs the checks of the plugin's files: here where it is installed, else in the
# node image Volumio's major.
with_node() {
  if command -v node >/dev/null 2>&1; then
    node "$@"
  else
    docker run --rm -i -v "$ROOT:/src:ro" -w /src node:20-bookworm-slim node "$@"
  fi
}
echo "check: plugin"
with_node --check plugin/index.js
with_node - <<'JS'
const fs = require('fs');
// A key twice in one object parses, and the second silently wins: refused.
function noDuplicates(file, text) {
  const stack = [];
  const re = /"(?:[^"\\]|\\.)*"\s*:|[{}[\]]/g;
  let m;
  while ((m = re.exec(text))) {
    const t = m[0];
    if (t === '{') stack.push(new Set());
    else if (t === '[') stack.push(null);
    else if (t === '}' || t === ']') stack.pop();
    else {
      const key = t.slice(0, t.lastIndexOf(':')).trim();
      const top = stack[stack.length - 1];
      if (top) {
        if (top.has(key)) { console.error(file + ': ' + key + ' twice in one object'); process.exit(1); }
        top.add(key);
      }
    }
  }
}
const files = ['plugin/package.json', 'plugin/config.json', 'plugin/UIConfig.json']
  .concat(fs.readdirSync('plugin/i18n').map((f) => 'plugin/i18n/' + f));
for (const file of files) {
  const text = fs.readFileSync(file, 'utf8');
  try { JSON.parse(text); } catch (e) { console.error(file + ': ' + e.message); process.exit(1); }
  noDuplicates(file, text);
}
const en = Object.keys(JSON.parse(fs.readFileSync('plugin/i18n/strings_en.json', 'utf8')));
const ui = fs.readFileSync('plugin/UIConfig.json', 'utf8').match(/TRANSLATE\.[A-Z_]+/g).map((k) => k.slice(10));
const unknown = ui.filter((k) => !en.includes(k));
if (unknown.length) { console.error('UIConfig.json: not in strings_en.json: ' + unknown.join(', ')); process.exit(1); }
for (const file of fs.readdirSync('plugin/i18n')) {
  const strings = JSON.parse(fs.readFileSync('plugin/i18n/' + file, 'utf8'));
  const keys = Object.keys(strings);
  if (keys.join('\n') !== en.join('\n')) { console.error(file + ': not the English keys in the English order'); process.exit(1); }
  const empty = keys.filter((k) => String(strings[k]).trim() === '');
  if (empty.length) { console.error(file + ': empty in ' + empty.join(', ')); process.exit(1); }
}
console.log('i18n: ' + (fs.readdirSync('plugin/i18n').length - 1) + ' translations carry the ' + en.length + ' English keys');
JS

echo "check: version"
version=$(sed -n 's/^ *"version": "\(.*\)",*$/\1/p' plugin/package.json | head -n1)
grep -q "^## \[$version\]" CHANGELOG.md || { echo "check: CHANGELOG.md has no section for $version" >&2; exit 1; }
grep -q "\"changelog\": \"v$version:" plugin/package.json || { echo "check: the store changelog in plugin/package.json is not for $version" >&2; exit 1; }

echo "check: actionlint"
if command -v actionlint >/dev/null 2>&1; then
  actionlint -no-color .github/workflows/*.yml
elif command -v docker >/dev/null 2>&1; then
  docker run --rm -v "$ROOT:/repo:ro" -w /repo rhysd/actionlint:latest -no-color .github/workflows/ci.yml .github/workflows/release.yml
else
  echo "check: neither actionlint nor docker is here; the workflows were not linted"
fi
echo "check: clean"
