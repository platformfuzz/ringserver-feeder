#!/usr/bin/env bash
# Open a PR when GHCR has a newer x.y.z tag than lab/docker-compose.yml.
set -euo pipefail

IMAGE="platformfuzz/ringserver"
COMPOSE="${COMPOSE:-lab/docker-compose.yml}"
REPO="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}"

current="$(sed -nE 's|^[[:space:]]*image: ghcr.io/platformfuzz/ringserver:([0-9]+\.[0-9]+\.[0-9]+).*|\1|p' "$COMPOSE" | head -n1)"
if [[ -z "$current" ]]; then
  echo "could not parse a x.y.z pin from ghcr.io/platformfuzz/ringserver in ${COMPOSE}" >&2
  exit 1
fi

token="$(
  curl -fsS "https://ghcr.io/token?service=ghcr.io&scope=repository:${IMAGE}:pull" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])'
)"
tags_json="$(curl -fsS -H "Authorization: Bearer ${token}" "https://ghcr.io/v2/${IMAGE}/tags/list")"
newest="$(
  printf '%s' "$tags_json" | python3 -c '
import json, re, sys
data = json.load(sys.stdin)
pat = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
vers = [t for t in data.get("tags") or [] if pat.match(t)]
if not vers:
    sys.exit("no x.y.z tags on ghcr.io/platformfuzz/ringserver")
vers.sort(key=lambda v: tuple(int(p) for p in v.split(".")))
print(vers[-1])
'
)"

echo "compose pin=${current} newest_ghcr=${newest}"

if python3 -c '
import sys
cur = tuple(int(p) for p in sys.argv[1].split("."))
new = tuple(int(p) for p in sys.argv[2].split("."))
raise SystemExit(0 if new <= cur else 1)
' "$current" "$newest"; then
  echo "already on newest x.y.z tag"
  exit 0
fi

branch="chore/bump-ringserver-${newest}"
open_prs="$(gh pr list --repo "$REPO" --head "$branch" --state open --json number --jq 'length')"
if [[ "${open_prs}" != "0" ]]; then
  echo "open PR already exists for ${branch}"
  exit 0
fi

pr_body="$(cat <<EOF
## Summary

- Bump \`ghcr.io/platformfuzz/ringserver\` in \`lab/docker-compose.yml\` from \`${current}\` to \`${newest}\` so Integration re-runs against the new serve-layer tag.

## Test plan

- [ ] Integration \`test\` job passes
- [ ] Lab prove.sh reports matching replica packet IDs
EOF
)"

if git ls-remote --exit-code origin "refs/heads/${branch}" >/dev/null 2>&1; then
  echo "remote branch ${branch} already exists; opening PR if missing"
  gh pr create --repo "$REPO" --base main --head "$branch" \
    --title "chore(deps): bump ringserver from ${current} to ${newest}" \
    --body "$pr_body"
  exit 0
fi

sed -i "s|ghcr.io/platformfuzz/ringserver:${current}|ghcr.io/platformfuzz/ringserver:${newest}|g" "$COMPOSE"

if [[ -n "${APP_SLUG:-}" ]]; then
  git config user.name "${APP_SLUG}[bot]"
  git config user.email "${APP_SLUG}[bot]@users.noreply.github.com"
else
  git config user.name "github-actions[bot]"
  git config user.email "41898282+github-actions[bot]@users.noreply.github.com"
fi
git checkout -b "$branch"
git add "$COMPOSE"
git commit -m "$(cat <<EOF
chore(deps): bump ringserver from ${current} to ${newest}

Track the new GHCR x.y.z tag so Integration tests the released
serve image rather than staying pinned to ${current}.
EOF
)"
git push -u origin HEAD

gh pr create --repo "$REPO" --base main --head "$branch" \
  --title "chore(deps): bump ringserver from ${current} to ${newest}" \
  --body "$pr_body"
